---
name: gadget-muse-apps
description: >-
  Write, install and manage apps on a Muse gadget's round screen: small Lua 5.4 apps that Muse writes
  on its VM, installs with app.install, checks with app.control and talks to with app.send. Use the
  gadget's own guide (app.api) for the API and its limits.
---

# Muse Gadget Apps: Write, Install and Manage Apps on the Gadget

Use this skill when the user asks for an app on their Muse gadget's screen (a timer, a counter, a dashboard, a small game), or asks to change, check, start, stop or remove one.

## Identify the Device

- The gadget's Link commands include `app.api`, `app.install` and `app.control`: only a gadget whose firmware has Muse apps advertises them.
- `app.api` answers with `api` 1, the round `screen` (466 x 466 px, `safe` 330), its `fonts`, `limits`, `storage` and installed `apps`.

## Prerequisites

- A Muse gadget with a round screen and firmware with Muse apps, linked to this VM.
- Muse can write files in `workspace/apps/` on this VM. If `app.api` answers `"delivered":"inline"`, the guide and the timer example are in its answer (`guide_text`, `example_files`) instead of in `workspace/apps/_guide/`.

## Workflow

This guide is for Muse, writing apps for a Muse gadget with a round screen. It comes from the gadget's own firmware: `app.api` writes it to `workspace/apps/_guide/README.md` on your VM, with a working example app in `workspace/apps/_guide/timer/`. Read it once before your first app, and come back to the API reference and the common errors as you work.

## What a gadget app is

- A small Lua 5.4 program that draws on the gadget's round screen: 466 x 466 px, of which only the circle of radius 233 about the centre is seen. `ui.SAFE` (330) is the side of the square inside that circle: keep text and buttons in it, and put only rings, arcs and decoration out towards the edge.
- One app runs at a time. It runs on the gadget itself: Muse installs it and can talk to it, and it keeps working without Muse, the VM or Wi-Fi.
- The user opens apps from the launcher (a long press on the watch face; it lists the four most recently used). A right swipe puts the app behind, where it keeps running. A voice turn puts the app behind too, and brings it back when the turn ends. An alarm brings its app to the front, and starts it first if it isn't running.
- Installed apps are kept in the gadget's flash and outlive a restart. A gadget without the apps partition keeps them in RAM until it restarts: results then say `"storage":"ram"` with a hint.
- Right after a restart the apps start once the gadget has registered with Muse, or 20 s after the restart at the latest. Until then the launcher says "Loading apps..." and the `app.*` commands answer `unavailable`: try again in a few seconds.
- The gadget stops an app that runs away; it doesn't restart for one. Each callback has a budget of Lua VM instructions (1,000,000, about 0.4 s on the gadget; 2,000,000 for main.lua's top level and `start()`) and 1 s of time, the Lua heap is capped at the manifest's `memory_kb`, and an app that uses more than 40% of the CPU for 5 s is stopped. Those are checked as the app's Lua runs; a call held up inside the gadget's own code is bounded by a supervisor instead. A call that hasn't returned after 3 s marks the app hung, and the gadget says so on screen.
- The Lua is sandboxed. It has the base library, `coroutine`, `table`, `string`, `math` and `utf8`, and the gadget's modules below. There is no `io`, `debug`, `load`, `dofile` or `collectgarbage`, and `require` loads only the app's own files. `string.match`, `string.gmatch` and `string.gsub` raise an error (patterns can run without bound); `string.find` searches plain text only. Globals are strict: reading one that was never set is an error, with a suggestion when a name is close. Declare everything `local`: a global set to nil is gone, and reading it again is that error. `setmetatable` refuses a metatable with `__gc`, and to-be-closed variables (`local x <close> = ...`) are not available.

## Files and the manifest

An app is a folder named after its id: you write it on your VM as `workspace/apps/<id>/`, with `manifest.json`, `main.lua` and any files the manifest lists. Paths are relative to the folder: `a-z 0-9 _ . / -`, at most 64 characters, no `..` and no part that starts with a dot. A file's type is its ending: `.lua` (Lua source text), `.json` (must parse) or `.bin` (an LVGL image, see Images); there is no other kind.

manifest.json, a JSON object of at most 8 KB:

- `id` (required): `^[a-z][a-z0-9_]{1,23}$`, the folder's name.
- `name` (required): 1 to 24 bytes, shown in the launcher.
- `version` (optional): up to 23 bytes, like `"1.0.0"`.
- `api` (required): `1`, a JSON integer (`1.0` isn't one).
- `main` (optional): the main `.lua` file, `"main.lua"` if left out.
- `files` (optional): every other file, like `["util.lua", "assets/bell.bin"]`: modules for `require`, `.json` data and images.
- `icon` (optional): one of `files`, a square `.bin` image at most 160 x 160 (120 x 120 looks best), shown in the launcher. See Icons.
- `permissions` (optional): any of `sound` (for `sound.chirp`), `alarm` (the `alarm` module), `store` (the `store` module) and `keep_awake` (for `sys.keep_awake`). `net`, `secrets` and `muse` aren't available in this firmware yet.
- `memory_kb` (optional): the Lua heap, a whole number from 64 to 1024; 256 if left out.
- `messages` (optional): up to 16 event names, each with a description under 200 characters: the events Muse may send with `app.send`. Describe the data each takes and what it replies.

Any other key is ignored with a warning. Limits: 32 files; 256 KB for a `.lua` file, 32 KB for a `.json` file, 512 KB for an image and 1.5 MB for all the images; 2 MB for the whole app. A running app has at most 128 widgets, 16 timers, 4 alarms (8 on the gadget) and 16 KB in its store. `app.api` returns these numbers as `limits`.

An app's files and its Lua heap share the gadget's memory, and the gadget keeps a reserve of it for itself: `app.api`'s `limits.room_kb` is what an app's files and its `memory_kb` may take together right now. With Muse's session up, the 2 MB of files and a 1024 KB heap don't both fit. While an app runs its memory is taken, so installing the app that runs again needs room for two copies of its files until the new one starts: stop it first when `room_kb` is short. A launch while another app runs counts what that app frees when it ends: it is refused up front, with the app that runs left running, when it wouldn't fit even then, or when its files don't fit beside that app now.

The timer example's manifest, shortened:

```json
{
  "id": "timer",
  "name": "Timer",
  "version": "1.0.0",
  "api": 1,
  "permissions": ["sound", "alarm", "store"],
  "memory_kb": 128,
  "messages": {
    "start": "Start a countdown. data: {seconds: whole number 1..5999}. Replies {state, remaining} in seconds.",
    "status": "Change nothing. Replies {state, remaining} in seconds; state is idle, running, paused or done."
  }
}
```

## App shape

`main.lua` returns a table of handlers. Every handler is optional:

```lua
local app = {}

function app.start() end               -- once, after main.lua's top level: build the screen, read the store
function app.stop() end                -- before the app ends (a stop, a reinstall, sys.exit): store what you need
function app.show() end                -- the app came to the front
function app.hide() end                -- the app went behind (a swipe, a voice turn, the launcher)
function app.message(event, data) end  -- Muse's app.send: return the reply
function app.alarm(name) end           -- an alarm this app armed has fired

return app
```

- The handlers are read once, when `main.lua` returns: changing the table later changes nothing. Any other key is a warning.
- `main.lua`'s top level can already draw, arm timers and read the store (it is loaded before `main.lua` runs). Timers run once `start()` has returned.
- An app launched in front starts shown; `show()` and `hide()` tell it each time that changes.
- An error in any handler or callback crashes the app, except in `message()`, where it fails that message only and the app keeps running. A crash shows a card on the gadget, and `app.control status` has the error with its traceback and the log.
- `print(...)` and `sys.log(...)` write lines to the app's log (each line cut at 200 bytes). `app.control logs` returns the last 50.
- `require("util")` loads `util.lua` (one of the manifest's files) once, and returns what it returned.

## API reference

### json

- `json.encode(v)`: v as JSON text. A table with the keys 1..n is an array, any other table an object (`{}` is `{}`). It refuses functions, NaN and infinity, strings that aren't UTF-8 and nesting deeper than 16.
- `json.decode(s)`: the value of the JSON text s, read strictly (no comments, no trailing commas; at most 32 KB and 16 levels). Integers stay integers, and null is `json.null`.
- `json.null` stands for null, in tables that must keep one.

### ui

- `ui.label{...}`: a line or block of text. Returns the widget.
- `ui.button{...}`: a button with text on it; give it a "tap" handler.
- `ui.box{...}`: a rectangle with a background, corners and a border, and the parent of the widgets made with `parent = box` (boxes nest at most 4 deep). A box without `bg` (or a `border`) is invisible: it has no background until you give it one.
- `ui.arc{...}`: a ring, or part of one, filled from start_angle by value between min and max. Give it `w` and `h`: without them it draws nothing.
- `ui.bar{...}`: a straight progress bar, value between min and max. Give it `w` and `h`: without them it draws nothing.
- `ui.image{...}`: one of the app's images (`src`, a path of the manifest's files).
- `ui.bg(color)`: the screen's background colour (black at first).
- `ui.clear()`: removes every widget.
- `ui.polar(deg, r)`: the x and y offsets, as integers, of the point r px from the centre at deg clock degrees, for a widget with `align = "center"`: `ui.polar(90, 100)` is `100, 0`, and `ui.polar(0, 100)` is `0, -100`.
- `ui.W` and `ui.H` are 466, `ui.R` is 233 and `ui.SAFE` is 330.
- `w:set{...}` changes properties, `w:get(name)` reads one (colours as `"#rrggbb"`), `w:on(event, fn)` calls `fn(w, event)` for the event (nil removes it), and `w:delete()` removes the widget and everything inside it. `set` and `on` return w, so calls chain.
- `ui.label` properties: align, x, y, w, h, hidden, opacity, parent, text, font, color, text_align, wrap; events: tap, long_press, press, release
- `ui.button` properties: align, x, y, w, h, hidden, opacity, parent, text, font, color, bg, radius, border, border_color; events: tap, long_press, press, release
- `ui.box` properties: align, x, y, w, h, hidden, opacity, parent, bg, radius, border, border_color; events: tap, long_press, press, release
- `ui.arc` properties: align, x, y, w, h, hidden, opacity, parent, min, max, value, start_angle, end_angle, thickness, color, track_color; events: tap, long_press, press, release
- `ui.bar` properties: align, x, y, w, h, hidden, opacity, parent, min, max, value, color, track_color, radius; events: tap, long_press, press, release
- `ui.image` properties: align, x, y, w, h, hidden, opacity, parent, src, scale; events: tap, long_press, press, release

The values:

- `align` is one of center, top, bottom, left, right, top_left, top_right, bottom_left, bottom_right (default center), and `x`, `y` are offsets from that point in the parent.
- `w` and `h` are 1 to 466. Leave them out and the widget sizes to its content: a label to its text, an image to its picture, and a button with no `w` and `h` is only as big as its text, with no padding. An arc or a bar has no content of its own, so without `w` and `h` it draws nothing, with no error or warning. `w:get("w")` and `w:get("h")` are nil while the widget sizes to its content: `get` reads what the app set, not the laid-out size (an install's `ui` has that).
- `hidden` and `wrap` are booleans (`wrap` breaks a label's text at its `w`). `opacity` is 0 to 100.
- `text` is a string (or a number) of at most 256 bytes of UTF-8. `font` is one of sans14, sans16, sans20 (the default), sans28, mono8, mono16, digits48 and digits96; the two digit fonts hold only `0-9 : . - + %` and the space. `text_align` is left, center (the default) or right.
- Colours (`color`, `bg`, `border_color`, `track_color`) are `"#rgb"`, `"#rrggbb"` or `0xRRGGBB`; `bg` also takes `"none"`. Text and arcs default to white, button backgrounds and tracks to `"#262626"`.
- `radius` is 0 to 233, or `"circle"`. `border` is 0 to 20. `thickness` (of an arc) is 1 to 64, 12 by default.
- `min`, `max` and `value` default to 0, 100 and 0, with min less than max; value is kept between them. `start_angle` and `end_angle` are clock degrees, 0 to 360: 0 is 12 o'clock, and they go clockwise.
- `parent` is a box, and can be given only when the widget is made. Widgets inside a box lay out in its content area: a border moves them in by its width, and LVGL clips what sticks out of the box. A box may have three boxes above it; a label, button, arc, bar or image may go in the fourth. `scale` is a percentage, 10 to 400.
- Numbers are rounded (halves away from zero) and clamped to these ranges, and an infinity is clamped too; NaN is an error. A value of the wrong type is an error; so is an unknown property or name, and that error lists the valid ones.
- A widget's handle is the same object for as long as the app holds it, so it can be a table key, and a handler gets the very handle the app holds. `tostring(w)` is like `label#3` (`label#3 (deleted)` once it is gone), and `getmetatable(w)` is the string `muse.widget`.

Touches. A tap is heard as press, release, tap, in that order; a long press as press, long_press, release, with no tap. A right swipe that begins on a widget puts the app behind: the widget hears press, then release, and no tap. A left swipe that begins on a widget is heard as press, release, tap. A widget that is deleted, or cleared with `ui.clear()`, while it is pressed never hears its release.

### time

- `time.now()`: the wall clock in epoch milliseconds, an integer; nil until the gadget has set its clock.
- `time.mono()`: milliseconds since the gadget started, on a clock that never jumps. Measure elapsed time with it.
- `time.synced()`: whether the wall clock is set.
- `time.date(ms)`: `{year, month, day, hour, min, sec, wday, yday}` of ms (default now) in local time; wday 1 is Sunday. Without ms it needs the clock set.
- `time.format(fmt, ms)`: fmt with `%Y %m %d %H %M %S %I %p %a %A %b %B %j %y %e %%` for ms (default now), in local time; at most 128 bytes.

### os

- `os.time(t)`: epoch seconds (seconds since start until the clock is set); with a table `{year, month, day, hour, min, sec}`, that local time.
- `os.date(fmt, s)`: Lua's, with `time.format`'s specifiers, `"!"` first for UTC and `"*t"` for a table.
- `os.clock()`: seconds since the gadget started, a float.
- `os.difftime(a, b)`: a - b.

### timer

- `timer.after(ms, fn)`: calls `fn()` once, ms from now. Returns a handle.
- `timer.every(ms, fn)`: calls `fn()` every ms. Returns a handle.
- `timer.cancel(h)`: cancels the timer, as `h:cancel()` does. Either may be called again, and from the timer's own callback.

ms is a whole number from 40 to 86400000. At most 16 timers at once. No timer runs while the screen is dark: when it lights again each timer that came due runs once, not once for each run it missed. Every timer goes when the app stops.

### alarm

These need the `alarm` permission. An alarm belongs to the gadget, not to the app: it fires while the screen sleeps and while the app isn't running. When it fires, the gadget brings the app to the front (starting it first if it isn't running) and calls `alarm(name)`; by default it also lights the screen and rings.

- `alarm.after(name, ms, {sound = true, wake = true})`: arms the alarm name (1 to 23 characters of a-z, 0-9 and _) to fire ms from now (1 to 604800000, a week). Arming a name that is armed replaces it. sound rings every 1.5 s for up to a minute, until `sound.stop()`; wake lights the screen. Both default to true.
- `alarm.cancel(name)`: disarms it (and ends its ring if it has fired and isn't delivered yet).
- `alarm.remaining(name)`: the milliseconds left, rounded up: 0 when it has fired and `alarm(name)` hasn't run yet, nil when there is no such alarm.
- `alarm.list()`: `{{name = "t", remaining = 4200}, ...}`, the earliest first.

At most 4 alarms an app and 8 on the gadget. One app's alarms may fire at most 6 times in a minute: when they fire more than 6 times within a minute, all of its alarms are cancelled, with no event to tell it (the gadget's log says `alarm: <id> fired more than 6 alarms in a minute; its alarms are cancelled`). Use `timer.every` for anything that repeats, and alarms for rare set times.

An alarm outlives a restart of the gadget once the gadget has stored it, about a second after it is armed; one armed before the gadget has set its clock is kept once the clock is set. After a restart the stored alarms come back once the gadget has set its clock and its apps have started (up to 20 s after the restart, so an alarm due before then fires that late), and until then `alarm.remaining` is nil for them; an app that arms or cancels one of those names first has its way. On a gadget that keeps its apps in RAM, no alarm outlives a restart.

What an app that uses alarms must guard against (the timer example shows the first five):

- `alarm.after` refuses 0 ms: arm a time you saved with `math.max(1, ms)`.
- An alarm that has fired and whose `alarm(name)` hasn't run yet reads 0 from `alarm.remaining(name)`, and one that is gone (delivered, cancelled, or cancelled by the limit above) reads nil: 0 and nil both mean it has ended. A countdown whose alarm reads nil has ended, even if `alarm(name)` never came.
- Arm first, then change your state: `local ok, why = pcall(alarm.after, name, ms, opts)`, the pcall of `alarm.after` itself so that `why` is the gadget's words. When it refuses (no free slot, no memory), leave the state, the store and the screen as they were, and show why.
- In `alarm(name)`, check `alarm.remaining(name)`: a name armed again before the firing was delivered is still counting.
- The ring is the gadget's, and `sound.stop()` ends any app's ring: call it only when the ring is your alarm's (it reads 0, or `alarm(name)` has just run).
- An app that an alarm started and that crashes in `start()` or `alarm()` loses all of its alarms.

### sound

- `sound.chirp()`: one chirp, at the gadget's volume. Needs the `sound` permission.
- `sound.stop()`: ends the ring of an alarm, whichever app's it is. No permission.

### store

These need the `store` permission. The values are the app's own, kept by id: they outlive a stop, a crash, a restart and a reinstall, and go when the app is removed. (A gadget that keeps its apps in RAM keeps their values in RAM too, until it restarts.)

- `store.get(key, default)`: a copy of the value, or default (nil if not given) when there is no such key.
- `store.set(key, value)`: keeps a copy of value: anything `json.encode` takes. nil removes the key.
- `store.remove(key)`: removes it.

A key is a string of 1 to 32 characters; a value may be nested 16 levels deep, and all the values together may take 16 KB as JSON. The store's cache lives outside the Lua heap: it doesn't count against `memory_kb` or show in `sys.mem()`. The store is saved 2 s after the last change (10 s at the latest, however often the app writes) and when the app ends, so a restart loses at most the last 10 s of changes (2 s for an app that pauses between writes). On flash, saves can come less often when the app writes a lot, to spare the flash.

### sys

- `sys.battery()`: `{pct = 0 to 100, charging = bool, usb = bool}`; pct is -1 when the gadget can't tell.
- `sys.wifi()`: `{connected = bool, ssid = "..."}`.
- `sys.show()`: asks for the front. Allowed in `message()` and `alarm()`, and in any callback within 10 s of a tap.
- `sys.keep_awake(on)`: true keeps the screen lit while the app is in front; false lets it sleep. Needs the `keep_awake` permission.
- `sys.has(name)`: whether the gadget has the module name (ui, time, timer, alarm, store, sound, json, sys).
- `sys.mem()`: `{used_kb, peak_kb, limit_kb}` of the Lua heap.
- `sys.exit()`: the app ends once this callback has returned: `stop()` runs and the store is saved.
- `sys.log(...)`: the same as `print(...)`.
- `sys.api` is 1, the manifest's api this gadget speaks.

### string

- `string.split(s, sep)`: the pieces of s between each sep (plain text), empty pieces kept: `("a,,b"):split(",")` is `{"a", "", "b"}`.
- `string.trim(s)`: s without its leading and trailing whitespace.

The rest of Lua's `string`, `table`, `math` and `utf8` libraries is there, with the limits of the sandbox above: search with `string.find(s, text, 1, true)`, and parse data with `string.split` or `json.decode`.

## Patterns

- Elapsed time: keep `local started = time.mono()` and show `time.mono() - started`. Never count the runs of `timer.every`: none run while the screen is dark.
- Anything at a set time is an alarm, not a timer: alarms fire while the screen sleeps, while the app isn't running and after a restart. Read the time left with `alarm.remaining`, as the timer example does. Keep alarms for rare set times: one that repeats every few seconds runs into the alarm section's limit, so tick with `timer.every`.
- Keep state in the store: `store.set` as it changes, and read it back in `start()`. A stop, a crash, a restart or a reinstall starts the app from `main.lua` again; only the store and the alarms remain. Check what you read: the store outlives a reinstall, so it may hold what an older version wrote.
- Lay out with `align = "center"` and x, y offsets inside `ui.SAFE`. Place things on a ring with `ui.polar`, and make an edge ring with an arc of `w = 440, h = 440`. Install results warn of any widget that goes past the round edge.
- Tick only while shown: start `timer.every` when the app is shown (in `start()` and in `show()`) and cancel it in `hide()`, as the timer example's `ticking()` does. An app behind needs no redraws.
- Answer `message()` with a small table (`{state = "running", remaining = 290}`), and raise `error("...")` with what to send instead for an event or data you can't take.

## Images

Images are LVGL v9 binary images (`.bin`), made with LVGL's own converter:

1. `pip install pypng lz4`, and take `LVGLImage.py` from the `scripts/` folder of LVGL v9.5.0.
2. `python3 LVGLImage.py --ofmt BIN --cf RGB565A8 --compress NONE -o assets --name bell bell.png` writes `assets/bell.bin`.
3. List `"assets/bell.bin"` in the manifest's files, write it to `workspace/apps/<id>/assets/bell.bin`, and draw it with `ui.image{src = "assets/bell.bin"}`.

- The formats are RGB565, RGB565A8 (colour with transparency) and A8. An A8 image draws as a white mask: for colour use RGB565A8. Images aren't compressed, and of the flags only premultiplied alpha is taken (LVGLImage.py's output passes).
- An image is at most 466 x 466 px and 512 KB, and an app's images 1.5 MB together.
- `scale` is a percentage: a scaled image's box is the scaled size. A w and h you give a scaled image are its box: the picture is centred in it and can spill past it. A scaled picture lands within 1 px of its box (2 px for an odd size at 400%), and a side that scales to under about 1 px draws nothing.

## Icons

The launcher shows each app by its icon.

- Make one with every app: a `.bin` image the manifest names with `icon` and lists in `files`, square and at most 160 x 160 px (120 x 120 looks best).
- Draw one bold, simple shape that reads at a small size, on a transparent background.
- Convert it like any other image, with `--cf RGB565A8` (see Images).
- An app without an icon shows as a coloured circle with the first letter of its name.
- When the user asks for "an icon for X", reinstall X with one: draw it, convert it, add `"icon"` and the file to the manifest, and install X again.

## The loop

1. Call `app.api`. It writes this guide and the timer example to `workspace/apps/_guide/`, and returns what the gadget is: `api`, `screen`, `fonts`, `limits`, `storage` and the installed `apps`. If the VM refuses the files, it returns the guide in `guide_text` and the example in `example_files` instead.
2. Write the app on your VM: `workspace/apps/<id>/manifest.json`, `main.lua` and the files it lists. Start from the timer example.
3. Call `app.install` with the id. The gadget reads the files itself, checks them, compiles, stores and runs the app in front, and returns `ok` with its `state`, `log`, `ui` (every widget with where it was laid out) and `warnings` (unknown keys, widgets past the round edge); or an error with a `code` (invalid_package, read_failed, compile_failed with the lines around the error, crashed with the traceback and the log, busy) and a message that says what to fix.
4. Fix and install again: an install replaces the app, and its store stays.
5. Check it with `app.control`: `status` (state, the error with its traceback, the last 50 log lines, memory, CPU, the widgets), `logs`, and `snapshot`, a picture of the screen that you open and look at.

`app.control` takes `action` and, for all but list, `id`:

- `list`: every installed app with its name, version, state, last error and messages.
- `status`, `logs`: as above. An id the gadget doesn't have is `not_found`.
- `launch`: runs the app in front (or brings it to the front if it runs) and returns its status.
- `stop`: stops it, if it is the one running.
- `remove`: deletes the app with its store and alarms. Stop it first if it runs.
- `snapshot`: saves a JPEG of the gadget's screen to `workspace/apps/<id>/snapshot.jpg` on this VM (open it to see the screen) and returns its `path`, `w`, `h`, `bytes`, `front` (whether the app is what is on screen: the picture is whatever is), the app's `ui` and `warnings` (widgets past the round edge). `scale: 2` makes it half size. A dark screen is woken for it. A snapshot needs room in the gadget's memory: when there isn't any it is refused before anything is drawn (`out_of_memory`), and a full-size one says when `scale: 2` would fit. The app that runs keeps running either way.

## Messages

- The manifest's `messages` list the events the app takes and describe their data and replies. Muse sees them in `app.control list`, and sends one with `app.send` (`id`, `event`, optional `data`, an object, and `show`).
- The gadget calls `message(event, data)`, with data as a Lua value (a table for an object; nil for none). What it returns, as JSON, is the reply (null for nothing). `error("...")` makes the reply that error, and the app keeps running.
- Check data before you use it: `type(data) == "table"` first (indexing anything else is an error), and note that `math.tointeger` also takes numeric strings like `"90"`.
- `sys.show()` inside `message()` brings the app to the front; `show = true` in `app.send` does the same once the voice turn ends.
- A message has the budget of any callback (1,000,000 instructions, 1 s): do the work, store what changed, reply.

## Common errors

What the gadget says, and what to do. `<...>` stands for a name or number in the message.

- `manifest: this gadget supports api 1 (manifest says api <n>)`: set `"api": 1`.
- `manifest: api is missing (this gadget supports api 1)`: add `"api": 1`.
- `manifest: id '<id>' must match ^[a-z][a-z0-9_]{1,23}$ and the folder name '<folder>'`: make the id lower case and the same as the folder's name.
- `manifest: name must be 1 to 24 characters`: give a short `name`.
- `manifest: files[<n>] '<path>' has an unsupported type (use .lua, .json or .bin)`: convert the file (images with LVGLImage.py), or leave it out.
- `manifest: icon '<path>' is not one of the app's files`: list the icon's file in `files`, or fix the path in `icon`.
- `manifest: icon '<path>' must be a .bin image`: name a `.bin` made with LVGLImage.py (see Icons).
- `icon: <w>x<h> must be square, at most 160x160`: redraw the icon square and at most 160 x 160 px (120 x 120), and convert it again.
- `manifest: unknown permission '<name>' (valid: sound, alarm, store, keep_awake)`: use one of those.
- `manifest: permission '<name>' is not available in this firmware yet (api 1)`: do without it.
- `manifest: memory_kb must be a whole number from 64 to <max>`: pick a whole number in range, or leave it out (256).
- `manifest: messages must map event names to descriptions (at most 16, each under 200 characters)`: shorten them.
- `unknown key '<key>' ignored (did you mean '<key>'?)`: a warning; fix the key's spelling.
- `reading <path>: VM answered HTTP 404`: write the file to `workspace/apps/<id>/<path>`, or take it out of `files`.
- `<path> is larger than <n> KB`: split the module, or shrink the image.
- `the app is larger than 2 MB`: shrink the images or the code.
- `images add up to more than 1.5 MB`: use fewer or smaller images.
- `files[<n>] <path> is a compiled chunk: install Lua source text`: install the `.lua` source, not luac's output.
- `compile failed: <file>:<line>: <Lua's message>`: a syntax error; the result shows the line and the lines around it.
- `crashed in start(): <error>`: `start()` raised an error (an install's result says `installed but ` first); read the traceback and the log, fix and install again.
- `install timed out after 90 s (reading <path>)`: the VM was slow or the session dropped; call `app.install` again.
- `busy: another install is running`: wait for it to finish, then install again.
- `image: not an LVGL v9 image file (use LVGLImage.py --ofmt BIN)`: convert the picture with LVGLImage.py.
- `image: compressed images aren't supported`: convert with `--compress NONE`.
- `image: flags 0x<nnnn> aren't supported (only premultiplied alpha)`: convert again with LVGLImage.py, without other flags.
- `image: colour format 0x<nn> isn't RGB565, RGB565A8 or A8`: convert with `--cf RGB565A8`.
- `image: <w>x<h> is larger than the 466x466 screen`: scale the picture down first.
- `unknown global '<name>' (did you mean '<name>'?)`: a typo, or a global never set; declare it `local` first.
- `string.<name> is not available in Muse apps (api 1): use string.find(s, text, 1, true), string.split or json.decode`: no patterns; search plain text.
- `'<close>' variables are not available in Muse apps`: use a plain local and close it yourself.
- `<main> must return a table of handlers: local app = {} … return app`: end `main.lua` with `return app`.
- `require: '<name>' is not one of this app's files (manifest.json "files": [...])`: list the module in `files`.
- `<main> has no message(event, data) handler`: add `app.message` to take `app.send`.
- `stopped: ran more than <n> VM instructions in one callback (use timer.every for long work)`: split the work across timer callbacks.
- `stopped: one callback ran longer than 1 s`: the same: do less in one call.
- `stopped: used more than 40% of the CPU for 5 s`: tick less often, and only while shown.
- `throttled: using more than 40% of the CPU`: a warning before that stop: do less per tick.
- `out of memory: this app may use <n> KB (memory_kb in manifest.json)`: raise `memory_kb`, or keep less in tables.
- `out of memory: the gadget is short of memory (this app's heap had reached <n> KB)`: the gadget ran out before the app reached its `memory_kb`: keep less in tables, use smaller images, or ask for less `memory_kb` (`app.api`'s `limits.room_kb` says what an app may take now).
- `ui: no more than 128 widgets per app`: reuse widgets with `w:set{...}` rather than making new ones.
- `widget was deleted`: a handle of a widget that `w:delete()` or `ui.clear()` removed; make it again.
- `<kind>#<n> '<text>' extends <n> px past the round edge`: a warning: move the widget in, or make it smaller.
- `layout not checked: the screen was asleep`: a warning: the screen was dark, so the install didn't check the layout (the round-edge warnings above); take an `app.control` `snapshot`, which wakes the screen, and read its `warnings`.
- `ui.<kind>: src '<path>' is not one of this app's images (manifest.json "files")`: list the `.bin` in `files`.
- `ui.<kind>: boxes nest at most 4 deep`: flatten the layout; a label, button, arc, bar or image may still go in the fourth box.
- `<module>: add "<permission>" to "permissions" in manifest.json`: add the permission (`alarm`, `store`, `sound` or `keep_awake`).
- `alarm: no more than 4 alarms per app`: cancel one first, or reuse its name.
- `alarm: the gadget has no free alarm slots (8 in all); cancel one`: other apps hold the rest: cancel one of this app's, or try later.
- `alarm: the gadget is short of memory; try again`: try again later; leave the state as it was meanwhile.
- `alarm.after: ms must be a whole number from 1 to 604800000`: arm a time you saved with `math.max(1, ms)`.
- `alarm: <id> fired more than 6 alarms in a minute; its alarms are cancelled`: in the gadget's log, with every alarm of the app gone; use `timer.every` for repeats, and treat an alarm that reads nil as ended.
- `timer: no more than 16 timers per app`: cancel timers you no longer need; one `timer.every` can drive several things.
- `store: more than 16 KB stored; remove keys you don't need`: store less.
- `store.set: nested deeper than 16`: flatten the value.
- `store: the gadget's app storage is full; values weren't saved`: store less, or remove apps you don't use.
- `store: saving less often to spare the gadget's flash`: a log line, not an error: write the store less often.
- `sys.show: allowed only in message() or alarm(), or within 10 s of a tap`: call it from those, or after a tap.
- `time.date: the clock isn't set yet`: check `time.synced()` first, or pass ms.
- `<id> was running during two restarts in a row; reinstall it to run it again`: the gadget restarted twice while the app ran; fix what made it restart, and install it again.
- `the stored copy of <id> is damaged (<file>: <why>); install it again`: install it again.
- `not enough room on the gadget for <id>: <n> KB free, <n> KB needed; remove an app you don't use`: remove an app with `app.control`.
- `the gadget keeps at most 32 apps; remove one first`: remove an app with `app.control`.
- `the gadget is short of memory for <id>: <n> KB free, <n> KB needed; restart it or install a smaller app`: lower `memory_kb` or the images, or stop the app that runs first.
- `the gadget is short of memory for <id>: <n> KB free once <id> ends, <n> KB needed; restart the gadget or install a smaller app`: even with the app that runs stopped there isn't room: lower `memory_kb` or the images. The app that runs keeps running.
- `the gadget is short of memory to load <id> while <id> runs: <n> KB free, <n> KB needed; stop <id>, then launch <id>`: there is room once the app that runs has stopped: stop it with `app.control`, then launch the app again.
- `reading <file>: out of memory; <id> is running, and an update needs room for two copies of it: stop it, then install again`: stop the app with `app.control`, then install it again.
- `the app stopped responding; no app can run until its stuck call returns or the gadget restarts`: a hung call; ask the user to restart the gadget.
- `the app task is busy; try again`: try again in a moment.
- `apps are starting; try again in a few seconds`: the gadget has just restarted: try again in a few seconds.
- `the gadget is short of memory right now; try again in a few seconds`: try again in a few seconds.
- `not enough memory for a full-size snapshot right now; take it at scale 2`: take the snapshot again with `scale: 2`.
- `not enough memory for a snapshot while the app runs; stop it (app.control stop) or make it use less memory, then try again`: the running app holds the room a snapshot needs: stop it with `app.control`, or give it smaller images or a smaller `memory_kb`, and take the snapshot again.
- `not enough memory for a snapshot right now`: try again in a few seconds.
- `the gadget is busy with another app job (an install, a launch or the guide); try again in a few seconds`: try again once the install has finished.

## Verify the Result

- `app.install` answers `ok` with `state` `running`; read its `warnings` and `log`, and fix what they point at.
- `app.control status` shows the app `running` with an empty `last_error`; `app.control logs` shows what it printed.
- Take `app.control snapshot` and open the `snapshot.jpg` it saves, or ask the user to look at the gadget.

## Limits

- One app runs at a time. An app has at most 32 files and 2 MB, 128 widgets, 16 timers, 4 alarms and 16 KB of store (`app.api`'s `limits`).
- In api 1 an app has no network, secrets or Muse calls of its own: data reaches it only through `app.send`.
- Write an app only in `workspace/apps/<id>/`, and remove an app only when the user asks.

## Sources

- The gadget's guide, `workspace/apps/_guide/README.md`, which `app.api` writes from the firmware, and its timer example in `workspace/apps/_guide/timer/`.
- [LVGL v9.5.0 image converter](https://github.com/lvgl/lvgl/blob/v9.5.0/scripts/LVGLImage.py)
- [Lua 5.4 reference manual](https://www.lua.org/manual/5.4/)
