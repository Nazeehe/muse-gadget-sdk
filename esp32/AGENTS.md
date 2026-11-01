<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# AGENTS.md

How to build this firmware and flash it onto your own ESP32 board. See
`README.md` for a shorter human overview.

## What this is

The ESP32 Device SDK: the ESP-IDF firmware (`muse-gadget`) powering Muse
Home Link. It pairs with a phone over BLE, joins Wi-Fi, and holds an encrypted
Noise session to a VM, with an optional home-network tunnel. `main/main.c` is the entry
point and `main/app.c` holds most of the logic. `components/muse` adds a
avatar/voice/settings UI on boards with a display.

## Prerequisites

- **ESP-IDF v6.0.1**, installed and activated (`. $IDF_PATH/export.sh`) so
  `idf.py` is on `PATH`. Other versions are unsupported. Errors about missing
  IDF headers such as `gcm.h` or `gpio_ll.h` usually mean the wrong IDF tag.
- Python 3 and a C/C++ compiler (`cc`, `c++`) for the host tests.
- A USB data cable. Flashing needs access to the serial port. A sandboxed
  agent usually has to run flash and monitor commands outside the sandbox.

## Supported boards

Board overlays live in `devices/`, which has a `README.md` listing each board's
hardware, features, and where to buy it, and an `AGENTS.md` recipe for adding
one. Its Vendor sources table lists each vendor's code for its boards: read it
before adding a feature to one.

| Board | Target | Overlay(s) after `sdkconfig.defaults` | Helper |
|---|---|---|---|
| ESP32-C5 DevKitC-1 (default) | `esp32c5` | none | `tools/board.sh devkit` |
| ESP32-C6 devkit without PSRAM | `esp32c6` | `devices/sdkconfig.c6-nopsram` | `tools/board.sh c6-nopsram` |
| ESP32-2424S012C 1.28" round display | `esp32c3` | `devices/sdkconfig.esp32-2424s012c` | `tools/board.sh esp32-2424s012c` |
| Espressif ESP32-S3-DevKitC-1 v1.1 (N8R8) | `esp32s3` | `devices/sdkconfig.espressif-s3-devkitc-1` | `tools/board.sh espressif-s3-devkitc-1` |
| ideaspark ESP32 + 1.9" ST7789 | `esp32` | `devices/sdkconfig.ideaspark` | `tools/board.sh ideaspark` |
| Waveshare ESP32-C6-LCD-1.47 | `esp32c6` | `devices/sdkconfig.waveshare-c6-lcd-147` | `tools/board.sh waveshare-c6-lcd-147` |
| Waveshare ESP32-S3-1.54inch-ePaper V2 | `esp32s3` | `devices/sdkconfig.waveshare-s3-epaper-154` | `tools/board.sh waveshare-s3-epaper-154` |
| Seeed SenseCAP Indicator | `esp32s3` | `devices/sdkconfig.sensecap-indicator` | `tools/board.sh sensecap-indicator` |
| Seeed reTerminal E1001 | `esp32s3` | `devices/sdkconfig.reterminal-e1001` | `tools/board.sh reterminal-e1001` |
| Seeed reTerminal E1002 | `esp32s3` | `devices/sdkconfig.reterminal-e1002` | `tools/board.sh reterminal-e1002` |
| Seeed reTerminal Sticky (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.reterminal-sticky` | `tools/board.sh reterminal-sticky` |
| Seeed reTerminal Sticky Link status (experimental) | `esp32s3` | `devices/sdkconfig.reterminal-sticky-link` | `tools/board.sh reterminal-sticky-link` |
| Home Assistant Voice Preview Edition | `esp32s3` | `devices/sdkconfig.home-assistant-voice` | `tools/board.sh home-assistant-voice` |
| Seeed reSpeaker Lite with XIAO ESP32-S3 (experimental) | `esp32s3` | `devices/sdkconfig.seeed-respeaker-lite` | `tools/board.sh seeed-respeaker-lite` |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175c` | manual (below) |
| Waveshare ESP32-S3-Touch-AMOLED-1.75 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175` | manual (below) |
| Waveshare ESP32-S3-Touch-AMOLED-2.16 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-216` | `tools/muse/board.sh build s3-216` |
| Espressif ESP-Mosaico (experimental; ESP-IDF master, see `devices/README.md`) | `esp32s31` | `devices/sdkconfig.muse;devices/sdkconfig.muse-esp-mosaico;devices/sdkconfig.muse-esp-mosaico-iris` | `tools/muse/board.sh build mosaico` |
| OSTB EchoEar-2ST (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-ostb-echoear-2st` | `tools/muse/board.sh build echoear` |
| Espressif ESP32-S3-BOX-3 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-espressif-box-3` | `tools/muse/board.sh build box3` |
| AIPI Lite | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-aipi` | manual |
| Waveshare ESP32-C6-Touch-AMOLED-1.8 | `esp32c6` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-c6-18` | manual |
| Waveshare ESP32-C6-Touch-AMOLED-2.06 | `esp32c6` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-c6-206` | `tools/muse/board.sh build c6-206` |
| Seeed SenseCAP Watcher | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-sensecap-watcher` | manual |
| M5Stack Cardputer ADV (experimental) | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-cardputer-adv` | `tools/muse/board.sh build cardputer-adv` |
| M5Stack StickS3 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-sticks3` | manual |
| M5Stack StopWatch | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stopwatch` | manual |
| Seeed XIAO ESP32-S3 1.47 Touch Display | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-seeed-xiao-s3-147` | `tools/muse/board.sh build xiao147` |
| M5Stack CoreS3 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-cores3` | `tools/muse/board.sh build cores3` |
| Freenove FNK0104B | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-fnk0104b` | `tools/muse/board.sh build fnk0104b` |
| VN ESP32-S3 1.83-inch NV3023 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-vn-s3-183` | `tools/muse/board.sh build vn183` |
| Guition JC3248W535 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-guition-jc3248w535` | `tools/muse/board.sh build jc3248w535` |
| Waveshare ESP32-S3-Touch-LCD-7 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-lcd7` | `tools/muse/board.sh build lcd7` |
| Waveshare ESP32-S3-Touch-LCD-1.28 | `esp32s3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-lcd128` | `tools/muse/board.sh build lcd128` |
| M5Stack StickC Plus2 | `esp32` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stickc-plus2` | manual |
| M5Stack Core2 (v1.0) | `esp32` | `devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-core2` | `tools/muse/board.sh build core2` |
| FoloToy AI Passport (experimental) | `esp32c3` | `devices/sdkconfig.muse;devices/sdkconfig.muse-ai-passport` | `tools/muse/board.sh build ai-passport` |

The default profile expects the C5 DevKitC-1: an addressable status LED on
GPIO27, the BOOT button on GPIO28 (active low), 8 MB flash and quad PSRAM.
The DevKitC-1's LED takes red first (`CONFIG_HOMEHUB_LED_RGB_ORDER`, on by
default). If yours shows green as red and amber as green, it takes green first
like most WS2812s: turn the option off in `idf.py menuconfig`, or in
`build-devkit/sdkconfig`, and rebuild.

## Build

Every application build needs the user's SDK token (`mgst_…`, from gadgets.muse.ai >
Account > SDK tokens). Ask for it, then set `CONFIG_GADGET_SDK_TOKEN="mgst_…"`
in that build directory's `sdkconfig` (or with `idf.py menuconfig`) before
building. Without it the build warns, and the gadget will stop pairing once
Muse requires tokens. Never commit the token or print it in full.

The opt-in `CONFIG_MUSE_POWER_TEST` image is an isolated board-load
characterization harness, not a pairing application, and needs no SDK token or
network credentials. Power measurement starts at `docs/power/README.md`. Read
`docs/power/ppk2.md` before any PPK2 use (authorization and fixture gates,
continuous source ownership, USB unplug/replug), and
`docs/power/sensecap-watcher.md` for the Watcher's build and procedure. Never
enable source output with unknown wiring, a connected battery, or
USB-charger backfeed into PPK2. Other boards reuse the shared
`muse_power_test_run()` core through `boards/muse_power_test_board.h` hooks
and board-owned matrices, driven by `tools/power/power_sweep.py`; do not fork
or rename the core for the next board.

### DevKitC-1 (default)

```sh
idf.py build                    # -> build/muse-gadget.bin, config in build/sdkconfig
```

### Other boards, with the helper

`tools/board.sh BOARD [build|flash|monitor|flash-monitor] [PORT]` sets the
target and overlays and gives each board its own `build-<board>/` directory
and `build-<board>/sdkconfig`:

```sh
tools/board.sh sensecap-indicator build
tools/board.sh ideaspark flash-monitor /dev/cu.usbserial-110
```

Without a port it uses the only matching serial port, and fails if it finds
none or more than one. If `idf.py` is not on `PATH`, it sources `export.sh`
from `$IDF_PATH`, `~/esp/esp-idf-v6.0.1`, `~/esp/esp-idf-v6` or `~/esp/esp-idf`.
The Sticky's Muse and Link status profiles build in `build-reterminal-sticky/`
and `build-reterminal-sticky-link/`. Set the SDK token in the chosen build
directory through `idf.py -B <dir> menuconfig`.

### Home Assistant Voice Preview Edition

`tools/board.sh home-assistant-voice build` builds a status-and-voice gadget:
the LED ring shows the status colours, holding the centre button records a
voice note that Muse answers in the app, and the dial sets the speaker volume
(shown on the ring, kept across restarts). It advertises as
`MuseGadget-ha-voice-XXXXXX`.

- The console and flashing go through the S3's own USB-Serial-JTAG, which
  shows up as `/dev/cu.usbmodem*` like a DevKitC-1. With both plugged in, pass
  the port, and check it with `python -m esptool read-mac` before flashing.
- Flashing replaces the vendor firmware on the S3 only. The XMOS audio chip
  keeps its own firmware; never reflash it.
- While voice chat is ready, the centre button is push-to-talk. Turn the mute
  switch on to get its setup role back (pairing confirmation, the 5 s
  factory-reset hold).

### Boards with the full UI, by hand

`tools/muse/board.sh build|flash <s3|s3n|s3-216|aipi|box3|c6|c6-206|watcher|sticks3|plus2|cardputer-adv|stopwatch|xiao147|cores3|core2|jc3248w535|lcd7|lcd128|vn183|ai-passport|echoear> [SERIAL|PORT]`
builds one board in `build-muse-<profile>/`, logs to
`/tmp/muse_build_<board>.log`, and clears `managed_components/` before and
after so it doesn't clash with other boards. When flashing, it finds the
board's port with `tools/muse/ports.py`, by the USB device behind it rather
than the port name, which changes when boards are re-cabled. With several
boards of one kind attached, pass the device's USB serial number (on the chip's
own USB serial port, its MAC) or the port; `tools/muse/ports.py --list` shows
them. It finds ESP-IDF the way `tools/board.sh` does, trying
`~/.espressif/esp-idf-v6.0.1` first. For an install anywhere else, set
`IDF_EXPORT` to its `export.sh`, e.g. in your shell profile:
`export IDF_EXPORT=/path/to/esp-idf/export.sh`. `tools/muse/avatar.py` builds
through `board.sh`, so it needs the same.

For bench testing, `MUSE_BENCH=1 tools/muse/board.sh build|flash ...` adds
`devices/sdkconfig.muse-bench` and uses `build-muse-<profile>-bench/`. That
turns on screenshots: `tools/muse/snap.py PORT KEYS OUT.png` sends bench keys
and saves the screen, and `>face=thinking` (or `idle`, `listening`,
`speaking`, `error`, `boot`, `off`, `happy`) in KEYS picks the avatar mode first.
Screenshots are off in normal builds because each one takes a buffer the size
of the screen. `>face=` works in any build. A bench build also takes touch from
the Mac, through LVGL's own input: `>tap X Y`, `>longpress X Y` and
`>swipe left|right|up` in KEYS (`snap.py PORT '>swipe left' OUT.png`) press and drag
as a finger would, on a touch screen that LVGL polls (`>swipe up` from the bottom
edge, at the middle, as the launcher's close takes it). `snap.py` sends `w` first,
since LVGL doesn't read its input while the screen sleeps on battery, and waits
for the script to finish before the picture (`snap.capture()` does both for
every caller). Each screenshot ends with a CRC-32 that `snap.py` checks, retaking
a capture a log line broke into. The firmware holds the console while it streams
one over USB, so the log lines an app or the system prints meanwhile come out after
END and can't land inside it; it stops, and lets the console go, if the host takes
nothing for 2 s. A UART console (the Watcher's) holds nothing, since its stream takes
about 40 s and no task could log for that long: the log can come between its rows,
and a capture it broke into is retaken. Or run `idf.py` directly:

```sh
idf.py -B build-muse-aipi -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-aipi/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-aipi" build
```

To flash, add `-p PORT flash` with the same `-B`, target and defaults
arguments, except on the SenseCAP Watcher: `idf.py flash` runs plain esptool,
which fails on its USB bridge (see "Flash it"). Build the Watcher with
`idf.py`, flash it with `tools/muse/board.sh flash watcher`, then
`idf.py … -p PORT monitor` as usual; reading from the bridge works. Muse builds use `partitions_muse.csv` and need 16 MB of flash or
more, except the StickS3, StickC Plus2, Cardputer ADV and AI Passport, which have 8 MB and use
`partitions_muse_8mb.csv`.

All boards share `managed_components/` and `dependencies.lock` in this
directory. If the component manager fails after you switch between a board
with the full UI and one without (lvgl is the usual offender), delete both and build again.

### OSTB EchoEar-2ST (experimental)

`tools/muse/board.sh build echoear` uses the measured ESP32-S3 rev 0.2 unit
with 16 MB DIO flash and 8 MB octal PSRAM at 80 MHz. Native USB Serial/JTAG
(`303a:1001`) is shared by several boards, so it does not identify the model
on its own. [EchoEar setup](devices/ostb-echoear-2st.md) records its pin map
and limits. The bottom button controls physical power; GPIO0 is not a talk
button. Talking and pairing confirmation use the touch screen. There's no
software power-off, so Settings has no Power off option.

The final port was tested on hardware: pairing, touch, recording and
playback, sleep and wake, and reconnect. See its setup guide for what wasn't
checked.

### A new board

Follow `devices/AGENTS.md`, which covers boards with a status light, a status
screen or the full UI from the overlay to verification. In short: copy the
closest overlay to `devices/sdkconfig.<yourboard>`, load it last, and add the
board to `devices/README.md`. At minimum set `CONFIG_IDF_TARGET`,
`CONFIG_HOMEHUB_BUTTON_GPIO`, the LED backend (`CONFIG_HOMEHUB_LED_BACKEND_*`,
or `..._NONE`), and the flash size and mode. Without PSRAM, also set
`CONFIG_SPIRAM=n`, `CONFIG_HOMEHUB_TUNNEL=n` and the mbedtls
internal-allocation options, as `devices/sdkconfig.ideaspark` does. Signed
apps on a classic ESP32 need chip revision 3.0 or later
(`CONFIG_ESP32_REV_MIN_3=y`).

## Flash

### Identify the board first

When you're asked to flash, work out which board is attached instead of asking
which one it is or assuming the default. Say which board you found and what told
you, then flash that profile. Getting it wrong writes another board's pin map,
flash size and status backend.

1. **Ask the running firmware.** A board already running this firmware names
   itself, and this needs no guessing:

   ```sh
   python3 tools/muse/chat.py --status     # {"board": "M5Stack StickS3", ...}
   ```

   It also logs the name once at startup, as `muse: board: <name>`, which
   `tools/muse/monitor.py PORT` captures because it resets the board first.
   `components/muse/muse_app.c` logs it; each `components/muse/boards/board_*.c`
   sets its `.name`. Boards with the full UI only: the other overlays don't log a name.

2. **Read the USB descriptor.** This works with no write access to the port and
   whatever the board is running, including vendor firmware or nothing.
   `ioreg -p IOUSB -l -w 0` on macOS, `lsusb -v` or `udevadm info /dev/ttyACM0`
   on Linux:

   | Descriptor | Board |
   |---|---|
   | Espressif `303a:1001`, "USB JTAG/serial debug unit" | the chip's own USB: C5, C6, S3 and the S3 boards with the full UI. Its serial number is the MAC |
   | CH340 (`1a86:7523`) | ideaspark, SenseCAP Indicator, reTerminal E1001 and E1002 |
   | CH9102 | M5Stack StickC Plus2 |
   | CH343 (`1a86:55d3`), "USB Single Serial" | Seeed reTerminal Sticky, Waveshare ESP32-S3-Touch-LCD-7 or ESP32-S3-Touch-LCD-1.28: `chip-id` and `flash-id` tell them apart (8 MB of PSRAM on the Sticky and the 7-inch, 2 MB on the 1.28-inch; 32 MB of flash on the Sticky, which `flash-id` shows) |
   | CH342, two `usbmodem` ports | SenseCAP Watcher: the S3 console is the one ending in `3`, the other is the Himax camera |

3. **Ask the chip.** When you can write to the port (this resets the board):

   ```sh
   python -m esptool -p PORT chip-id      # read-mac for the MAC
   ```

   The target narrows it a long way: `esp32c5` is the DevKitC-1, `esp32c6` the
   Waveshare C6 or a C6 devkit, `esp32` the ideaspark or the StickC Plus2.

4. **Fall back to a read-only capture.** If the board is mid-run and you can't
   write to the port, the `## Monitor` recipe below reads it without resetting,
   and a talk press logs the board's own button name as `muse_input: waking
   (<hint>)`:

   | Hint | Board |
   |---|---|
   | `top-right`, or `top-left` with live calls (`yellow` on older firmware) | M5Stack StopWatch |
   | `USR1` | Seeed XIAO ESP32-S3 1.47 Touch Display |
   | `power` | M5Stack CoreS3 |
   | `front` | M5Stack StickS3, or StickC Plus2 — tell them apart by the port: the StickS3 is native USB, the Plus2 is a CH9102 `usbserial` |
   | `top` | Waveshare ESP32-S3-Touch-AMOLED-1.75C |
   | `key` | Waveshare ESP32-S3-Touch-AMOLED-2.16 |
   | `bottom right` | AIPI Lite |
   | `wheel` | Seeed SenseCAP Watcher |
   | `boot` | Waveshare ESP32-C6-Touch-AMOLED-1.8, the ESP32-S3-Touch-AMOLED-1.75, the Guition JC3248W535 or the VN ESP32-S3 1.83-inch NV3023 — tell the C6 by its target (`esp32c6`), and the S3 boards by the `muse: board:` line a reset logs |

Ask the user only when these come up empty or contradict each other, and say
what you found and what's ambiguous rather than asking from scratch.

### Flash it

```sh
idf.py -p PORT flash                           # default DevKitC-1 build
tools/board.sh devkit flash PORT               # same, in build-devkit/
cd build && python -m esptool --chip esp32c5 -b 460800 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```

Typical ports: `/dev/cu.usbmodem*` or `/dev/ttyACM*` for native USB (C5, S3 and
C6 boards), and `/dev/cu.usbserial-*`, `/dev/cu.wchusbserial*` or `/dev/ttyUSB*`
for CH340 bridges (ideaspark, SenseCAP Indicator, reTerminal E1001 and E1002). On Linux,
add yourself to the `dialout` (or `uucp`) group. If the chip won't enter the
bootloader, hold BOOT, tap RESET, release BOOT, and flash again.

The M5Stack StickC Plus2's CH9102 bridge shows up as `/dev/cu.usbserial-*` or
`/dev/ttyACM*`. It drops out above 230400 baud, so `tools/muse/board.sh`
flashes it at 230400.

The SenseCAP Watcher's bottom USB-C port has a CH342 bridge with two ports,
both `/dev/cu.usbmodem*` on macOS. The ESP32-S3 console is the second (ending
in `3`); the first is the Himax camera chip. Muse overwrites the Watcher's
factory data, so back up its `nvsfactory` partition first (see
`devices/README.md`).

The CH342 has no flow control and drops bytes when esptool sends a whole
packet at once, at any baud and on any USB port. Plain esptool then fails with
`Failed to write to target RAM (result was 0107: Checksum error)` while
uploading its stub, or `0105: The format of the received message is invalid`
on the first flash block, by which point it has erased the bootloader. That
leaves the Watcher unable to boot until a flash succeeds; the ROM loader still
answers, so it isn't bricked. `tools/muse/board.sh flash watcher` goes through
`tools/muse/paced_esptool.py`, which takes esptool's arguments and sends 64
bytes at a time at the line rate, at 115200 baud. Use it in place of
`python -m esptool` for anything that writes to the Watcher. A lower baud or
another USB port doesn't help. Pacing works with macOS's built-in driver, so
WCH's driver isn't needed.

`board.sh` prints only esptool's last three lines, so a Watcher flash shows
nothing for about three minutes. Don't interrupt it, which leaves the board
unbootable again. To watch the progress, run the wrapper from the build
directory:

```sh
cd build-muse-sensecap-watcher
python ../tools/muse/paced_esptool.py --chip esp32s3 -p PORT -b 115200 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```

- `flash` writes the bootloader, the partition table (at `0x10000`, not the IDF
  default `0x8000`), otadata, phy_init and the app to `ota_0`. NVS is left
  alone, so pairing and Wi-Fi credentials survive a reflash.
- `idf.py -p PORT erase-flash` wipes everything, including pairing. Do this
  only when you mean to start from scratch.
- The app is signed with the committed `dev_signing_key.pem`, and Secure Boot
  is **not** enabled, so flashing never burns eFuses. Keep it that way: don't
  enable `CONFIG_SECURE_BOOT`, flash encryption, or
  `CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH` on a board you want to keep reflashing.

## Monitor

`idf.py -p PORT monitor` (Ctrl-] to quit) resets the board and needs an
interactive terminal. An agent without a TTY can do a read-only capture that
doesn't reset the board:

```py
import os, termios, time
fd = os.open("/dev/cu.usbmodem1101", os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[2] &= ~termios.HUPCL   # don't toggle DTR/RTS (which resets) on close
termios.tcsetattr(fd, termios.TCSANOW, attrs)
deadline = time.monotonic() + 10
while time.monotonic() < deadline:
    try:
        chunk = os.read(fd, 4096)
        if chunk:
            os.write(1, chunk)
    except BlockingIOError:
        time.sleep(0.05)
```

To capture from boot, use `tools/muse/monitor.py PORT [secs]`, which resets the
board first (it needs `pyserial`). A healthy boot logs
`link.main: Muse Gadget starting`. For a board with the full UI, `$(tools/muse/ports.py BOARD)`
gives the port, with BOARD as in `tools/muse/board.sh`.

## Update my avatar

When someone asks to put their own avatar on their board (or to update or
change it), run this from `esp32/` with the sandbox off (it uses the serial
port) and let it finish. It takes several minutes, mostly waiting for Muse:

```sh
python3 tools/muse/avatar.py                    # draw their avatar
python3 tools/muse/avatar.py --edit "CHANGE"    # change the one they have
```

It checks the board, asks their Muse for the renderer through the board,
saves it to `components/muse/avatar/muse_pixel.c` (gitignored), checks it on
the host, then builds and flashes. Progress and errors go to stderr. Relay the
line that starts with `Muse drew:` and the GIF paths. On failure, pass the
message on. The exit status says what kind of failure it was:

- **2**: no board, or it can't do this. If it says the board doesn't answer,
  rerun with `--board s3` or `--board aipi` (from the board's name, or ask) to
  flash firmware that can. On the C6 boards or Watcher, follow the manual steps in
  `tools/muse/AVATAR_RECIPE.md`.
- **3**: the board isn't on Wi-Fi, or it isn't paired in the Muse app and has
  no device token, so their Muse isn't connected. Tell them, and point them to
  the Muse app or `tools/muse/ble_setup.html`.
- **1**: Muse's file still fails after two fix rounds, or the build or flash
  failed. The previous avatar is in `muse_pixel.c.prev`, next to the new one.

Don't commit anything in `components/muse/avatar/`. To see what the board
says, run `python3 tools/muse/chat.py --status`. To ask their Muse something,
run `python3 tools/muse/chat.py "question"`.

The default avatar is in `avatar/`: its renderer (`muse_pixel.c`) and
its animation (`jollybot.gif`, and `happy_anim.c/.h` made from it by
`tools/gen_happy_anim.py`).

Third-party code keeps its upstream license and header: `minimp3.h` (CC0),
`main/pixel_font.c` (BSD-2-Clause, Adafruit),
`components/muse/boards/ostb_echoear_display_init.h` (MIT, 78/xiaozhi-esp32),
`main/epd_ssd1677.c` (MIT, Seeed-reTerminal-E10xx-Firmware),
`components/tinyh264/src` (Apache-2.0, the Android Open Source Project),
`components/libvpx_vp8/src` (BSD-3-Clause and `PATENTS`, the WebM Project) and
`components/lua/src` (MIT, Lua.org and PUC-Rio).
Each third-party file is listed under License in `README.md` and the root
`README.md`; add a new one to both.
Don't restyle them or replace their headers with the Apache one; `components/minimp3/README.md` says how to
update minimp3.

The Apache License doesn't cover the Jollybot avatar in `avatar/`. Its files
carry only a Meta copyright line; don't add the Apache header to them.

## First boot and pairing

The status LED (or the edge bars or avatar on display boards) shows the state:

| Colour | Meaning |
|---|---|
| orange, breathing | BLE advertising, waiting for setup |
| blue, breathing | pairing needs an explicit device press (screen on EchoEar-2ST) |
| blue | Wi-Fi or VM session coming up |
| green | connected (tunnel up, or control session up when the tunnel is off) |
| yellow, blinking | VM switching or connection lost |
| purple | unpaired |
| red, blinking | error |

Button (BOOT on the dev boards):

- **short press**: confirm a pending pairing, or reopen the setup window if
  setup isn't complete
- **hold for 5 s**: reset setup (unpair and forget Wi-Fi)

EchoEar-2ST confirms pending pairing with an explicit screen press; hold the
on-screen microphone to record and release it to send once paired. It has no
physical talk/setup button, so the GPIO button reset gesture does not apply.

The device advertises as `MuseGadget-XXXXXX` (`MuseGadget-Disp-XXXXXX` on the
ideaspark, SenseCAP Indicator and reTerminal E1001 and E1002 overlays, `MuseGadget-ha-voice-XXXXXX` on the
Voice PE, `MuseGadget-respeaker-XXXXXX` on the reSpeaker Lite). It uses
**community pairing v5**, so the phone app must support v5
and list community devices. Community pairing needs the explicit device press but has no
manufacturer attestation, and it doesn't stop an active man-in-the-middle.

Until it's paired, a board with the full UI shows that name on its screen, dim under the
state, so you can tell which gadget to pick in the Muse app. A screen too narrow
for the whole name shows the `XXXXXX` tail on its own, and a square 128 px
screen (AIPI Lite) leaves it out, the same as it leaves out the state. Once it's
paired (or has a token set by hand), the name goes, and the mic icon and the
touch boards' speaker button appear. They're hidden until then, since a press
can't reach Muse and there are no replies to mute. The StopWatch is an exception:
its dedicated microphone, speaker/mute and power rim icons stay visible before
pairing as well, identifying its three physical controls.

With `CONFIG_MUSE_CLOCK` (on for the Waveshare 1.75C), a touch board has a
pocket watch page left of Muse's face. The screen starts and wakes on it, and
the talk button there brings Muse's face up instead of recording. It takes the
time from `pool.ntp.org` once Wi-Fi joins, in `CONFIG_MUSE_CLOCK_TZ`, a POSIX
TZ string that defaults to UTC: set it in the build's `sdkconfig`. Bench
builds select the page with `>ui=clock`.

To skip BLE Wi-Fi provisioning while you iterate, set
`CONFIG_HOMEHUB_WIFI_SSID` and `CONFIG_HOMEHUB_WIFI_PASSWORD` in `menuconfig`.
The device still needs to be paired once for its token.

## Live voice calls (experimental)

`CONFIG_MUSE_REALTIME_VOICE` (off by default, "Live voice calls with Muse"
under Muse in menuconfig) adds calls beside voice notes on boards with Muse's
own voice session: the full-UI boards with PSRAM and the Voice PE (and
reSpeaker Lite) builds. On StopWatch, hold yellow to talk or interrupt, release
to keep the call open, and tap blue to hang up; voice notes are an explicit
Sound setting. With the option on, the StopWatch's yellow key is its talk
button and blue its speaker button (`board_m5stack_stopwatch.c`), swapped from
the default build: outside a call blue still mutes, and during one, or once a
press that will start one is queued, its press hangs up (`muse_input.c`). A
hang-up that can't be queued moves the input epoch, which ends the call. Other boards retain tap-to-call and hold-for-note. Link-only
builds, the DevKitC-1 default included, don't have the option and don't change.

- The call is a `POST /voice/realtime` over the voice session's Noise
  connection: `components/muse/muse_voice_call_core.c` has the protocol (a
  bounded JSON scanner, the 40 ms appends, the audio decode, the call's
  lifecycle and deadlines) with no IDF in it, and `muse_voice_call.cpp` runs
  it on the session task. `tests/test_muse_voice_call.py` covers both on the
  host with synthetic audio.
- The session connects with `app_id` (`CONFIG_MUSE_REALTIME_VOICE_APP_ID`,
  `hatch-web` by default): the VM takes the app identity from the connection,
  not from a request header, and refuses calls without one. With the option
  on, this applies to everything on that connection, voice notes and typed
  turns included.
- The voice session never sends `link.register`: Link's own control
  connection keeps the device's registration.
- Muse's greeting is off by default (`CONFIG_MUSE_REALTIME_VOICE_GREETING`).
- Every way out of a call sends `POST /voice/end-call` while the connection
  is up: closing the media alone doesn't end a call. Server policy may also
  end a call before the client's own limits. If the connection drops instead, the
  VM keeps the call through a disconnect grace (about 45 s, then up to 35 s
  more) before ending it, and in that window another connection of the same
  account can take the call over; end calls explicitly rather than by
  dropping. So a call whose end wasn't confirmed (the connection dropped
  first, or Muse didn't confirm the end-call) is kept as one pending cleanup:
  its end-call, unchanged, goes again when the session reconnects, before a
  new call starts (which waits for one try, at most 10 s), up to 3 tries and
  for at most 10 minutes; a refused (4xx) end-call isn't retried. It names the old call's id, so it can't end a new
  call, and nothing else of the old call is sent again. A new call's admission
  replaces the old one at Muse, which also ends the cleanup. The pending
  cleanup is in RAM only: if the gadget loses power or restarts first, the
  call stays until Muse's disconnect grace runs out. If Muse doesn't confirm the end-call
  (`{"ok":true,...}`), the session drops its connection once the media is
  down, which makes the VM end the call. The gadget hangs up after
  `CONFIG_MUSE_REALTIME_VOICE_IDLE_S` quiet seconds, and always at
  `CONFIG_MUSE_REALTIME_VOICE_MAX_S`. Held input, a caller mid-sentence (until
  that speech item's own stop) and a turn Muse is still working on aren't idle;
  silent keepalive audio is. Turns are tracked by `submission_message_id` and
  input `item_id`: a `turn_settled` retires its submission and the earlier ones
  for that item, never another item's or a later alias.
- On push-to-talk calls only replies to your latest hold play
  (`muse_vc_reply_gate_t`). The one exception: once that reply has finished,
  older work from before your last press reporting back (a delegated task's
  result, or the outcome after you answer a spoken approval) plays after it,
  with captions. The audio of a reply you cut off with a press never comes
  back, and older work arriving while a newer reply is still going is
  dropped. Approvals work by voice: Muse asks in the call and you hold to
  answer; the gadget has no approval card.
- Nothing plays and the mic doesn't open until `hatch.voice.session` names this
  call's exact `call_id` and a `session_id`; a missing or wrong one fails the
  call. Before that, an `error` event fails it too; after, only `session_dead`
  does.
- Caller and reply text go to captions only, never the log.
- Hands-free calls shut the mic while Muse is audibly talking, plus 300 ms.
  The Voice PE keeps it open (full duplex) when its XMOS reports echo-cancelled
  outputs at start-up.
- The VM must accept calls from the gadget's credentials. A `session_control`
  stop on this call's realtime stream ends the call: the client drains queued
  playback and explicitly ends it. The admission body's second line,
  `MUSE_VC_CAPABILITY_OFFER` (`hatch.voice.capability.offer` with
  `end_call_stop: 1`), tells Muse the gadget ends calls that way, so it can
  hang up when the caller says goodbye; the gadget has no channel for
  `ui.set.voice.end_call`. A VM that doesn't support the offer ignores it and
  sends no stop on goodbye, so a press, the idle limit or the length limit is
  what reliably ends a call there.

## Requests to Muse's VM from your code

`CONFIG_HOMEHUB_DAEMON_REQUESTS` (on by default, on every board, Link-only
builds included) lets firmware open its own HTTP requests to the VM's daemon
on Link's Noise session, with `noise_ctrl_req_open()`, `noise_ctrl_req_send()`
and `noise_ctrl_req_cancel()` in `main/noise_control.h`. Muse's own voice notes
and chat use the same calls. Each request gets a stream of its own; a refused,
reset or cancelled request never touches Link's control stream.
`tests/test_link_daemon_requests.py` runs them on the host with the Muse UI off.

Routes a gadget's credentials can reach, besides Link's own and the voice
call's `POST /voice/realtime` and `POST /voice/end-call`:

- `GET /chat/history`: the chat's messages. Ask for one at a time
  (`GET /chat/history?limit=1`) so the response stays under the frame bound
  below. A single message larger than that bound can't be fetched this way.
- `GET` and `HEAD /fs/raw/{path}`: read a file on the user's VM, one
  `Range: bytes=<first>-<last>` at a time, each under the frame bound below.
  The answer is a 206 with that range, a 416 at or past the end of the file, a
  404 (with a JSON error) for no such file, or a 200 with the whole file if the
  VM ignores Range (`main/vm_file.h` takes that for the first range only, and
  only if the file fits in the range buffer). `{path}` is relative to the
  user's home on the VM, percent-encoded but for its slashes, under the VM's
  existing path rules; nothing confines it to a workspace folder.
  `vm_file_open`, `vm_file_read` and `vm_file_read_all` read this way (one
  reader across the firmware), for `display.draw_file` and the apps' installs.
  `vm_file_open` refuses a path that is absolute or has a `..` folder
  (`vm_file_open: bad path`, as `vm_file_write` does); `display.draw_file`
  drops leading slashes first, as it always has.
- `POST /fs/upload`: write a file, as `multipart/form-data` in one streamed
  request (`Content-Type: multipart/form-data; boundary=<b>`). The text parts
  come first, in this order: `path` (relative to the user's home, as above),
  `overwrite` (`true`) and `create_parent` (`true`). Then the `file` part
  (`filename` is the path's last name, `Content-Type:
  application/octet-stream`) with the raw bytes: send them in pieces of 8 KB
  (4 KB on a Muse board without PSRAM), and end the body with the closing
  boundary. Pieces of that size matter: the session copies every send with
  `malloc`, and with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` (Muse boards) a
  copy of 4 KB or less is internal RAM, which on the S3 is the DMA RAM the
  Link's TX needs 16 KB of free, and up to 16 KB of sends are queued at a time;
  a larger piece's copy is in PSRAM. The VM writes a temporary file and renames
  it into place, so a file is replaced whole, and there is no append. A 2xx
  reply is JSON with `bytes_written`: compare it with what you sent; a refusal
  is `{"ok":false,"error":{"message":...}}` or the text of a gateway. The
  boundary must not appear in the path or the file. `vm_file_write()` in
  `main/vm_file.h` does all of this.
- `POST /fs/read` and `POST /fs/write` are the owner's routes, not a gadget's:
  its credentials get HTTP 403, `route not allowed for this credential's
  scopes`. Read with `/fs/raw` and write with `/fs/upload`.

There is no route to list a folder for gadgets.

Keep within the session's bounds:

- Up to 3 requests at once; another is refused (its callback gets -1).
- Send a body in pieces of at most 8 KB (4 KB on Muse boards without PSRAM,
  2 KB on the Cardputer ADV and AI Passport): a larger piece ends that request.
  Queued body bytes are capped at 16 KB with PSRAM and 4 KB without, and
  `noise_ctrl_req_send()` waits up to `wait_ms` for room. Each send is copied
  with `malloc`: on Muse boards a copy of 4 KB or less is internal (DMA) RAM, so
  prefer the largest piece.
- `noise_ctrl_req_cancel()` waits 100 ms for room in the session's queue and
  then gives up silently. A request whose body never ended (an upload you give
  up on) keeps one of the 3 streams until that cancel is queued, so use
  `noise_ctrl_req_cancel_wait()`, which says whether it was, and ask again.
- Each response frame has to fit the session's inbound buffer: 64 KB with
  PSRAM, 17 KB on Muse boards without it, 12 KB on other boards without it. A
  larger one ends the whole session, which reconnects, and the request with
  `NOISE_CTRL_REQ_TOO_LARGE`. The frame carries the response's head too, so ask
  for bodies of at most `noise_ctrl_req_max_body()` bytes (63,488 with PSRAM, as
  on the 1.75C; 15,360 on Muse boards without it; 10,240 on the others), as
  `vm_file` does with its ranges: read large files in parts.
- The callback runs on the session task: copy what you need and return. Do
  slow work, and any request that depends on its result, in a task of your
  own, as `on_ws_command()`'s asynchronous commands do. That includes waiting
  for a reply: `vm_file_read()`, `vm_file_read_all()` and `vm_file_write()` wait
  for answers that only the session task delivers, so they must never run on it.
  `vm_file_close()` waits for nothing and can. So does `vm_file_open()` in a
  build without the apps; with `CONFIG_MUSE_APPS` an open may wait, up to
  `VM_FILE_APP_WAIT_MS`, for an app's request to let go of the reader, so on
  the session task only the image download's open runs, and it never waits
  (`main/vm_file.h`).

## Video from a phone

`MUSE_VIDEO=1 tools/muse/board.sh build|flash <board>` (overlay
`devices/sdkconfig.muse-video`) adds video streaming (`CONFIG_MUSE_VIDEO`,
`CONFIG_MUSE_VIDEO_WIFI`): Muse Gadgets Test (`../android/musegadgetstest` for
Android, `../ios/musegadgetstest` for iPhone) on a phone on the same Wi-Fi streams
its camera, or a clip, as H.264 or VP8
(`CONFIG_MUSE_VIDEO_VP8`, on by default), and the board decodes it and
draws it full screen. `../protocols/VIDEO.md` has
the design, the Wi-Fi protocol and the numbers.

- **Boards:** those whose panel driver has `draw_frame` and that have PSRAM:
  the Waveshare S3 1.75C (tested) and 1.75, the StopWatch and the
  ESP-Mosaico. Another QSPI AMOLED driven by `boards/muse_lcd_bands.c` gets
  it with `.draw_frame = muse_lcd_bands_frame` and its panel gap
  (`muse_lcd_bands_set_gap`); `muse_lcd_bands_set_te` if the TE line is wired.
- **Pieces:** `components/tinyh264` and `components/libvpx_vp8` (the
  decoders, behind `muse_vdec.c`),
  `components/muse_vstream` (the receive ring and frame format any transport
  shares), `components/muse/muse_video.c` (decode and show tasks, pacing,
  bands, the view), `muse_yuv*.c/.S` (colour conversion and scaling),
  `muse_video_wifi.c` (TCP on port 7766, mDNS `_musevideo._tcp`) and
  `muse_video_wire.c` (its messages). A new transport fills a
  `muse_video_transport_t` and calls `muse_video_begin()`.
- **Using it:** Phone camera in Settings (touch boards) or the menu opens the
  view and asks the phone for a stream; any button closes it. The console's
  `>video` / `>video=0` do the same. The board takes a stream only while the
  view is open: the port is unauthenticated (`../protocols/VIDEO.md`, Trust).
  `CONFIG_MUSE_VIDEO_WIFI_START_ANYTIME` lifts that for automated benches;
  never ship it. Stats every 2 s on `muse_video`: decode time,
  received-to-shown, kb/s, shown/dropped/skipped, each core's load.
- **Radio:** while a stream is on, Wi-Fi stays out of modem sleep and BLE
  stops advertising (`muse_wifi_keep_awake`, `muse_link_ble_quiet`): with BLE
  advertising, the radio's scheduler gives BLE half of every beacon period
  and the stream's packets wait out that half (50 ms round trips instead of
  15, and the phone's bitrate falls to its floor).
- **Memory:** the ring (128 KB) and the decoders' memory are in PSRAM, and
  the decode tasks' stacks in main SRAM (`MALLOC_CAP_DMA`): the S31 also
  hands out LP RAM as heap once main SRAM runs low, several times slower, so
  keep anything per-frame out of plain internal `malloc`. The decode and show
  tasks take about 20 KB of internal RAM once the first stream starts.
  `CONFIG_MUSE_VIDEO_DECODER_IRAM` (off) puts the decoders' hot code in IRAM:
  44 KB (and 33 KB for VP8; `CONFIG_MUSE_VP8_IRAM` picks less or more) that a
  Wi-Fi + BLE + UI build on the S3 can't spare (LVGL's task then fails to
  start).
- **Test** with `tests/test_muse_video.py` (the ring and the Wi-Fi messages,
  and the wiring under the options), `tests/test_video_untrusted.py`
  (hostile sizes, the port's guards), `tests/test_tinyh264_pie.py` and
  `tests/test_libvpx_vp8.py`; on a board, with Muse Gadgets Test (Android:
  `adb logcat -s MuseCam`; iPhone: Xcode's console).
- **Untrusted input:** everything from the network is. Keep the checks in
  `../protocols/VIDEO.md`'s Trust section when changing the transport, the
  decoders' glue (`muse_vdec.c`) or the drawing (`fill()` in `muse_video.c`).

## Muse apps

`MUSE_APPS=1 tools/muse/board.sh build|flash s3` (overlay
`devices/sdkconfig.muse-apps`, `CONFIG_MUSE_APPS`) adds Muse apps: small Lua
5.4 programs that Muse writes on its VM and installs on the gadget over Link.
Each draws its own screen and runs in a sandbox with its own memory cap and CPU
budget, and keeps working without Muse, the VM or Wi-Fi. It's for the Waveshare
S3 1.75C only (`board.sh` refuses `MUSE_APPS=1` for other boards): the launcher
hangs off its pocket watch. Add `MUSE_BENCH=1` for the bench console
(screenshots, injected touch, `>mem`). Without the option nothing changes.

- **Pieces:** `components/lua` (Lua 5.4.9, unmodified) and, in
  `components/muse/apps/`: `apps_vm.c` (the sandboxed state, the budgets and
  the kill), `apps_rt.c` (handlers, timers, the duty cycle, the log),
  `apps_pkg.c` (the manifest and package rules), `apps_ui.c` and `apps_lv.c`
  (widgets: what Lua holds, and the LVGL task's apply step), `apps_svc.c`
  (`time`, `os`, `timer`, `alarm`, `sound`, `store`, `sys`), `apps_json.c`,
  `apps_task.c` (the app task and the supervisor that marks a hung call),
  `apps_io.c` (the I/O worker, which does every flash access), `apps_store.c`
  and `apps_values.c` (the apps' FAT and `apps_nvs`), `apps_icons.c` (the
  launcher's icons, read on the I/O worker), `apps_install.c` (the one
  install pipeline), `apps_alarm.c`, `apps_console.c` (the console's
  `>app.*`), `apps_control.c` (`app.api`, `app.control`), `apps_snapshot.c`
  (JPEG snapshots), `apps_vmio.c` (the `vm` module), `apps_net.c` (the network
  worker, which runs the `vm` module's requests over the VM ops),
  `apps_ask.c` (`muse.ask`), `apps_jpeg.c` (runtime JPEGs), the examples
  (`examples/`) and the digit fonts (`fonts/`); `main/apps_link.c` (the Link
  commands and the VM ops); `tools/muse/app.py` and `tools/muse/apphost/`
  (the host runner).
  `components/muse/apps/apps_guide.md` is the guide Muse reads, and
  `skills/gadget-muse-apps/SKILL.md` holds it for an agent.
- **Using it:** a long press on the pocket watch opens the launcher, a
  carousel of every app, the most recently used first; a tap on the app in the
  middle opens it and a tap on one at a side brings it to the middle; a swipe
  up that starts in the bottom 40 px of the screen (`APPS_LV_CLOSE_EDGE_PX`)
  closes it; a right swipe puts the app behind, where it keeps running. After
  the tap the launcher stays up, taking no touch, until the app's first frame,
  for at most 3 s (`APPS_LV_LAUNCH_HOLD_MS`), so the watch doesn't flash
  between them. Muse uses four Link commands:
  `app.api` (writes the guide, a timer example and the tally connected
  example to `workspace/apps/_guide/` on its VM and returns the gadget's limits),
  `app.install` (the gadget reads `workspace/apps/<id>/`
  from the VM, checks, compiles, stores and runs it), `app.control` (list,
  status, logs, launch, stop, remove, snapshot) and `app.send` (an event to the
  app's `message()`). From a laptop, `tools/muse/app.py check DIR` runs an app
  on the host, `push PORT DIR [--run] [--snap OUT.png]` installs it over USB,
  `watch PORT DIR` pushes it on every change and `logs PORT` follows its log.
  The console's own lines are `>app.put`, `>app.commit ID [run]`,
  `>app.launch ID`, `>app.stop`, `>app.status`, `>app.list` and
  `>app.send ID EVENT [JSON]`; `>app.status` takes no id and reports the app
  that runs. The first line after the port opens can be lost, so send `w`
  first and wait a second.
- **Launcher:** a carousel (`apps_lv.c`): the app in the middle is 160 px
  across (`APPS_LV_ICON_PX`), its neighbours 100/256 of that, 63 px
  (`APPS_LV_ICON_SIDE_SCALE`), 170 px apart (`APPS_LV_ITEM_PITCH`); each
  app's size follows its distance from the middle as the row scrolls, and a
  small dot marks the app that runs. A sideways swipe browses and snaps an app
  to the middle; a tap on the app in the middle or on its name opens it, a tap
  on a side app brings it to the middle, and a swipe up from the bottom edge
  or a tap on the background closes the launcher. Each app shows its icon, or a coloured
  circle with its name's first letter while it has none or its pixels are
  still coming. The guide asks for 160 x 160 icons, the size of the app in the
  middle: a smaller one is scaled up there and looks soft. The I/O worker
  reads the icons of the 5 apps around the middle (`APPS_ICONS_WINDOW`) into
  PSRAM as the middle moves, and a close frees them. Measured on the 1.75C on
  2026-10-30 with 10 apps, seven of them with icons (two of 160 px): `>mem`'s
  internal RAM free, its largest block and DMA free didn't move with the
  launcher open, scrolling or closed; free PSRAM fell by 154,512 bytes open on
  the first app (two icons in its window), by 198,720 on an app with three, by
  at most 228,200 while scrolling, and came back to within 16 bytes at the
  close. With an app of 1.3 MB running behind it (906 KB of Lua heap and a
  434 KB image) the launcher cost the same and its icons still loaded. A bench
  build logs `launcher frame <ms> ms` (the `muse_apps` tag) at each display
  refresh while the carousel scrolls: the time since the refresh before. Five
  flings across all 10 apps (two `>swipe left` or `>swipe right` back to back
  each) gave 150 frames, a median of 30 ms and a 95th percentile of 41 ms
  (the longest 51), inside the frame gate of 33 and 50 ms, so the carousel
  scales its icons live as they scroll. That median counts the snap's slow
  tail, 47 frames under 28 ms: the other 103, while a finger drags and as the
  snap starts, have a median of 33 ms against the 30 of all 150, the gate's
  own limit, so per-frame work added later has no margin to spend.
- **Boot:** the apps' storage waits for the Link (`muse_apps.h`). The app task
  mounts the FAT and `apps_nvs`, and the I/O worker that does it is made, once
  the reply to the first `link.register` has come, accepted or not
  (`noise_control.cpp`); at once when no Link session was started at boot (no
  Wi-Fi or pairing, or it couldn't connect: `app.c`); and 20 s after boot at
  the latest (`APPS_BOOT_DEADLINE_MS`, `apps_task.h`). The boot's lowest
  internal RAM falls between the Link's handshake and its registration, and
  this keeps the mount's internal allocations out of that window. Until the
  store is mounted the apps are starting: `app.*` answers `unavailable`, "apps
  are starting; try again in a few seconds", the launcher says "Loading
  apps...", and an alarm the store re-arms after a restart fires once it is
  mounted, up to the deadline late. A console `>app.*` line mounts it at once
  (the I/O worker's first step).
- **Limits:** a callback may run 1,000,000 Lua VM instructions and
  `main.lua`'s top level with `start()` 2,000,000, each in at most 1 s; the
  1.75C runs about 2.5 M instructions a second of the simplest loop (and,
  measured the same way, 2.1 M of table and call work), so that is about
  0.4 s and 0.8 s.
  `tests/muse_apps_fixtures/bench` measures the speed, and `APPS_BUDGET_*`
  (`apps_vm.h`) and `APPS_DUTY_INSTR_PER_S` (`apps_rt.h`) are set from it. A
  second with more than 1,000,000 instructions or 400 ms of calls is over 40%
  of the CPU: 3 such seconds in a row throttle the app's timers, 5 stop the
  app, and a call that hasn't returned after 3 s marks it hung.
- **Memory:** PSRAM holds the app task's 56 KB stack, the running app's Lua
  heap (its manifest's `memory_kb`, at most `MUSE_APPS_HEAP_MAX_KB`), the I/O
  worker's 8 KB stack, the network worker's 8 KB stack (`APPS_NET_STACK`; it is
  made by an app's first `vm` request and stays) and, while one is taken, a
  snapshot. The apps always
  leave 1 MB of PSRAM to the rest of the gadget (`APPS_RAM_RESERVE`,
  `apps_pkg.h`): Muse's session takes about 430 KB while it is up and about
  630 KB at the peak of a turn, and LVGL aborts when it can't allocate. So
  `apps_port_realloc` refuses growth below it (the app, an install's read or a
  load ends out of memory, cleanly), and `app.api` gives what is left as
  `limits.room_kb`. A launch is admitted twice before the app that runs is
  touched: its files and the reserve must fit in free PSRAM now, and its
  files, its heap and the reserve once the app that runs has ended (counting
  what that end frees: its Lua heap as the VM counts it and its package). A
  launch short of either is refused and the app that runs keeps running.
  Every run is admitted once more, after the app it replaces has gone, with
  room for its heap and the reserve. While the launcher is open its icons are
  in PSRAM too, through the same floor: up to 5 of at most 76,812 bytes each
  (`APPS_ICONS_WINDOW`, `APPS_ICON_MAX_BYTES`) and 2 more loading; a pick
  keeps only the 3 on the screen (`APPS_LV_HOLD_KEEP`) until the app's first
  frame, and a launch refused for memory closes the launcher, which frees
  them, and is tried once more. The launcher closes when the screen goes
  dark, which frees them, so it holds none while the gadget sleeps; while
  someone browses it, a launch it didn't pick (an alarm's, or one Muse asks
  for) and a snapshot are admitted with its icons counted, and can be refused
  for them. A snapshot is one block through
  the same floor (the screen's pixels, the picture masked in place and the
  JPEG's room: 565,392 bytes at scale 1 and 434,312 at scale 2, with 64 KB and
  40 KB allowed for the encoder's own buffers, also floored and counted with the
  block; the encode's log line says their peak, measured at 55,208 and 32,168
  bytes). It is admitted, before anything is drawn, only when free PSRAM and the
  largest free block each hold that above the reserve, and refused otherwise
  with `out_of_memory` (scale 2 suggested when it fits). An app task allocation
  the floor refuses while a snapshot holds its blocks waits for the snapshot,
  up to 1 s, then is tried once more. Measured, a snapshot holds its block for
  about 0.2 s (drawn and encoded) before cutting it to the JPEG, so a snapshot
  doesn't kill the app that runs. The stacks can be in PSRAM because the 1.75C's
  `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` and `CONFIG_SPIRAM_RODATA` keep the cache
  on while flash is written; `apps_io.c` refuses to build without them.
  Against the same build without the option, the image is 441,064 bytes
  bigger (Lua 101 KB, the JPEG encoder 30 KB, FAT 32 KB, the digit fonts
  27 KB) and an idle gadget has about 552 KB less free PSRAM, most of it that
  image, since the board runs code and constant data from PSRAM. Internal RAM
  is the tight one: about 2.0 KB less free 25 s after boot (measured, the
  median of 24 boots each). The state only one task touches with plain loads
  and stores is in PSRAM (`EXT_RAM_BSS_ATTR`; atomics and locks stay
  internal), and the overlay puts NVS's cache there
  (`CONFIG_NVS_ALLOCATE_CACHE_IN_SPIRAM`, which moves the cache, not what is
  stored). Its lowest since boot is set while the Link's session comes up and
  registers, before the apps' storage mounts (Boot, below). The apps' commands
  make the registration 3,967 bytes against 2,669, and an apps build prints it
  into PSRAM (`noise_control.cpp`): under 4 KB, malloc takes internal RAM. A
  build without the apps still prints its 2,669 bytes into a 3,072-byte malloc
  buffer, in internal RAM, so while the registration is sent it holds 3 KB
  more internal RAM than an apps build. That buffer is taken at the send,
  after the lowest is usually set, and measured it doesn't move the floor.
  Measured on the 1.75C on one day, over 24 boots each, the lowest was 12.6 KB
  at the median and 9.7 KB at the least with the apps, and 11.4 KB and 9.6 KB
  without; a build without the apps that printed its registration into PSRAM
  (for measurement only) gave 11.7 KB and 10.5 KB over 12. The apps build from
  before the mount waited (b944c60) gave 13.0 KB and 9.1 KB over 12 boots that
  day, and 15.4 KB and 9.1 KB over 24 on its own day: a run of boots moves the
  median by a few KB. Sampled every 40 ms, 11 of 12 apps boots set their
  lowest between the session's handshake and the registration's send, before
  the apps start. 22 of 24 boots fall under the Link's 16 KB TX margin, with
  or without the apps, in the Link's own setup: that floor is an open item of
  the Link's.
  The deepest stacks seen: the app task 13,012 bytes free of 57,344, the I/O
  worker 3,968 of 8,192, and the LVGL task 1,240 free
  after 128 widgets, Settings, the launcher, a crash card, a snapshot and a
  `display.draw_file`.
- **Storage:** `partitions_muse_apps.csv` is `partitions_muse.csv` with two
  partitions added and no offset moved: `apps`, a 6 MB FAT at `0x830000` (a
  folder per app), and `apps_nvs`, 64 KB at `0xE30000` (the apps' saved values
  and their alarms). A new partition table takes one flash over USB
  (`board.sh flash`); an update over the air can't write one. A gadget whose
  table has no `apps` partition keeps its apps in RAM until it restarts, and
  results say `"storage":"ram"` with the hint "reflash over USB once to keep
  apps". A build directory made before the overlay keeps its old table in its
  own `sdkconfig`: delete that file.
- **Connected apps:** an app can show data from Muse's VM and its
  connectors and act on it. The `vm` module (`apps_vmio.c`, whose requests the
  network worker `apps_net.c` runs over the VM ops) reads, watches and writes
  the app's folder on the VM, `workspace/apps/<id>/`; `muse.ask` (`apps_ask.c`)
  puts a question to Muse as a typed turn in the user's chat; `vm.read` of a
  JPEG (`apps_jpeg.c`) gives a runtime image, which `ui.image` can turn or spin.
  On the VM, a bash event hook that Muse writes with the app keeps the folder's
  `state/` current from the connectors and applies the app's commands, with no
  model in the loop. The guide's "Connected apps" section teaches the pattern:
  the folder, the heartbeat and the rev file, commands and acks, the hook
  template (the in-front gate, the loop, the call budget, dry runs) and the
  creation flow with its approvals. Its example is
  `components/muse/apps/examples/tally` (an app and its `hook.sh`, with no
  connector), which `app.api` writes to `workspace/apps/_guide/tally/` beside
  the timer; the build embeds it through copies named `tally_<name>`, since IDF
  names an embedded file's symbols after its name alone.
  `tests/test_muse_apps_hook.py` runs that hook under bash 4 or later with a
  stand-in for the VM's hook runtime, and `apphost --vm DIR` runs an app
  against a folder that stands in for its folder on the VM.
- **Connected apps, measured** on the 1.75C on 2026-11-01 (apps+bench build,
  with Muse on the user's VM):
  - `app.api` wrote the guide and its examples, 164,689 bytes, to the VM in
    1.3 s, well inside its 20 s (`MUSE_APPS_GUIDE_WRITE_MS`).
  - The tally, installed by Muse from the guide: "+1" was on screen 2.0 to
    2.7 s after the tap (the command's write, the hook's next check and the
    gadget's next read of the acks), "Ask" brought Muse's reply in about
    13 s, and the Lua heap peaked at 43 KB of its 64. With the app behind,
    each hook run ended at the in-front gate in 26 to 37 ms, every 5 s, with
    no connector call.
  - A VM read opens only while internal and DMA RAM each keep the Link's
    16 KB TX margin plus 4 KB (`VM_RESERVE_BYTES`, `main/vm_file.c`) and the
    request free, about 20.6 KB; below that the app's read gets "the gadget is
    short of memory". DMA free is 23.6 to 26.3 KB with an app watching or
    spinning, but on one boot it stayed at 20,419 bytes for hours after the
    Link lost its session and came back, and a watch read failed so. A
    restart gave 26,699.
  - A still picture under a spinning one: show it at scale 100. Each step of
    the spin redraws what is under the turning picture, and a scaled picture
    is drawn there through the same slow path as a turning one, about 18 to
    20 ms more a frame at any opacity. With a 180 px record spinning over the
    picture: unscaled (300 px, or 440 px) 32 to 33 ms frames (median; p95 35
    to 44) and taps of 148 to 170 ms; scaled to fill the screen (105% or 155%)
    51 to 53 ms and 182 to 216 ms; with no picture under it 29 ms and 134 ms.
    Reading a 300 px JPEG from the VM and decoding it took 564 ms, and 616 ms
    at 440 px.
  - A typed turn and a voice press both work while an app watches: the voice
    turn hides the app, and the app comes back in front when the turn ends.
- **Test** with `tests/test_muse_apps_*.py` (the VM and the sandbox, JSON,
  packages, widgets and the apply step, the services, the runtime and its duty
  cycle, the app task, the store, the install, the console and `app.py`, the
  Link commands and the guide, snapshots; the guide's quoted numbers are pinned
  to the headers), `tests/test_vm_file.py` and `tests/test_muse_bench.py`.
  They compile the apps' sources with ASan and UBSan where the compiler can,
  and need `CJSON_SOURCE_DIR` or a `managed_components/` from a build. On a
  board, `app.py push PORT tests/muse_apps_fixtures/<fixture> --run --snap
  OUT.png`, then `>app.status`, `>mem` and `tools/muse/snap.py`; a typed turn
  through `tools/muse/chat.py` checks the Link commands with Muse.
- **Untrusted input:** the package, the app's code and what `app.send` brings
  are all untrusted. The sandbox loads Lua source text only (bytecode is
  refused) and has no `io`, `debug`, `load`, `dofile` or `collectgarbage`, no
  string patterns, strict globals and `require` of the app's own files only;
  `setmetatable` refuses `__gc`, and every metatable C hands out is protected.
  Every value is checked before LVGL sees it, since LVGL asserts abort. Paths
  are `a-z 0-9 _ . / -`, at most 64 characters, in parts split by `/`: no part
  is empty, starts or ends with `.`, or has two dots in a row (so no `..`, and
  no leading or trailing `/`), and the install reads only
  `workspace/apps/<id>/`, a path the gadget makes. A file's type is its ending (`.lua` source text, `.json` that
  must parse, `.bin` an LVGL image whose header is checked), under per-file and
  per-app size limits. Keep these when changing the bindings
  (`apps_*_funcs[]`), the package rules (`apps_pkg.c`) or the install.

## Configuration gotchas

- `sdkconfig.defaults.esp32c5` is loaded after `sdkconfig.defaults` for C5
  builds only (ESP-IDF's target-specific defaults): it moves lwIP and Wi-Fi
  heap buffers to PSRAM, raises the TCP windows to 64 KiB and deepens the
  receive path to match (64 dynamic RX buffers, 12 queued out-of-order
  segments). Other chips keep 16 KiB windows in internal RAM;
  `cmake/validate_config.cmake` enforces both.
- Each build's generated `sdkconfig` lives in its build directory
  (`build/sdkconfig`, `build-<board>/sdkconfig`). Once it exists, it overrides
  later edits to `sdkconfig.defaults` or the overlays. After changing those,
  delete the generated `sdkconfig` (or the whole build directory) and rebuild.
  `cmake/validate_config.cmake` stops the build if a stale config has the old
  TCP buffer sizes or cJSON nesting limit.
- Edit per-build settings with `idf.py -B <dir> menuconfig` (under "ESP32
  Device SDK"). Don't commit a generated `sdkconfig`: it's gitignored for a reason.
- The e-ink UI's task stack (`muse_eink`) must be in internal RAM: Settings'
  handlers write NVS synchronously, and flash writes disable the cache. A PSRAM
  UI task stack asserts in `esp_task_stack_is_sane_cache_disabled()` when
  changing a setting. The framebuffer and font caches can remain in PSRAM.
- E-paper boards with the full UI (`CONFIG_MUSE_UI_FREEINK`, the Sticky so
  far) draw with [FreeInk](https://github.com/Free-Ink/freeink-sdk), not
  LVGL, which they don't build at all. To bring up another panel: give it a
  FreeInk board profile (or reuse one), select `MUSE_UI_FREEINK` and name the
  profile in `MUSE_FREEINK_DEVICE` (`components/muse/Kconfig`), and write a
  board file that supplies `touch_read`, buttons, audio and power and points
  `display_pause` at `muse_eink_display_pause` (`eink/muse_eink.h`); leave
  `display_start` and the other display hooks NULL. The UI's fonts are
  embedded TTFs (`eink/fonts/`, regenerated by `tools/muse/gen_eink_fonts.sh`).
  FreeInk is pinned to a commit in `components/muse/idf_component.yml`.
- OTA is off by default (`CONFIG_HOMEHUB_OTA_ENABLED=n`, version `999.0.0`),
  except on boards with the full UI. Set a version with `-DPROJECT_VER=1.0.0` or
  `version.txt`.
- BLE is shut down and its memory released once setup completes, except on
  boards with the full UI. `CONFIG_HOMEHUB_BLE_PERSISTENT` keeps NimBLE up
  beside Wi-Fi with a basic GATT service (`main/ble_basic.c`) advertised
  slowly; it costs roughly 40 KB of internal RAM for as long as the device
  runs (`tools/coexist/sdkconfig.ble-mem` wins most of it back on the C5).
  On the C5, `CONFIG_HOMEHUB_BLE_COEX` (`main/ble_coex.c`) keeps BLE
  advertising and connections from reserving half the radio's airtime once
  setup is complete, which doubles Wi-Fi throughput with BLE up. It relies
  on ESP-IDF's private coexistence status bits, so it builds on IDF v6.0.x
  only until the bits are rechecked on another version.
  `tools/coexist/` holds the overlays used to size it, and the data-path
  bench (`CONFIG_HOMEHUB_PIPELINE_BENCH`) with its results.
- With BLE persistent, `CONFIG_HOMEHUB_BLE_COMMANDS` (default on) lets Muse
  drive the Link's BLE: `ble.state`, `ble.power`, `ble.advertise`,
  `ble.scan`, `ble.connect`, `ble.disconnect`, `ble.discover`, `ble.read`,
  `ble.write` and `ble.subscribe` (`main/ble_commands.c` over the central in
  `main/ble_client.c`). They run one at a time on their own task. `ble.power`
  off stops the stack without releasing the controller's memory, so it can
  come back; the default build, which releases BLE after setup, can't.
  `tests/test_ble_commands.py` runs them against a fake client.
- On the C5 with PSRAM, `CONFIG_HOMEHUB_FAST_GCM` (default on there) wraps
  ESP-IDF's one-shot AES-GCM with `main/fast_gcm.c` for TLS and Noise. It
  takes about 19 KB of internal RAM (8-bit GHASH tables and the PSRAM staging
  buffer; see its Kconfig help). Turn it off to compare against IDF's;
  `tests/test_ghash32.py` checks the tables and its failure handling.
- Don't move offsets in `partitions.csv`. `prod_data` and `prod_bak` are fixed
  manufacturing locations, and the table offset of `0x10000` leaves room for a
  larger Secure Boot bootloader. Check the `check_sizes` line in the build
  output: app slots are 2 MB (4 MB on Muse).

## Adding a command

Muse calls a gadget's commands by name: the firmware lists them in
`link.register` and answers each `link.invoke`. A command you add lives in two
places, which must use the same name. (`device.health` and `device.ota` are
handled in `noise_control.cpp` itself; everything else goes through
`on_ws_command()`.)

1. **Advertise it** in `build_register_json()` in `main/noise_control.cpp`:
   `add_command(commands, "relay.set", "<description>", required, optional)`.
   `required` and `optional` map each parameter's name to `{type,
   description}` (`string_param()` makes a string one). Muse reads the
   descriptions, so say what the command does and what it returns. If it can
   take longer than the default 30 seconds, set its `timeout_ms`, as
   `device.discover` does.
2. **Handle it** in `on_ws_command()` in `main/app.c`. Return
   `{"ok": true, "payload": {...}}`, or `command_error(code, message)` for a
   failure. Only `ok`, `payload` (or a `payload_json` string) and the
   error's `message` reach the Muse. Always return a result: `NULL` reaches
   the Muse as a generic "command handler did not return a result" error.
   Validate the parameters yourself: `params` is `NULL` when the request has
   none, and the firmware doesn't check them against the advertised
   `required` and `optional`, so check each one's presence, type, length and
   allowed values.
3. **Don't block.** `on_ws_command()` runs on the Noise session's task, so
   anything slow (the network, a slow sensor, a camera) belongs in its own
   task. Copy `request_id`, `session_generation` and every parameter the task
   needs, strings included, into memory the task owns (`device.discover`
   uses `cJSON_Duplicate()`): the request is freed as soon as
   `on_ws_command()` returns. If an allocation or the task start fails, free
   what you allocated and return `command_error()`. Otherwise start the task
   and return `{"_async": true}`. The task then calls
   `noise_ctrl_send_command_result()` once, failures included, or the Muse
   waits out the timeout. It takes ownership of the result and frees it, so
   don't free or reuse it afterwards. `camera.capture` and `device.discover`
   work this way. A result from an earlier session is dropped.
4. **Gate it on a Kconfig option** in `main/Kconfig.projbuild` when it needs
   particular hardware, and wrap both places in the same `#if`, as
   `sensors.read` does with `CONFIG_HOMEHUB_SENSECAP_SENSORS`. Add new source
   files to `main/CMakeLists.txt`.
5. **Keep `link.register` small.** It's printed into at most 8 KB, and a
   device whose registration doesn't fit never registers.
6. **Add a host test** in `tests/`. `test_link_sensecap_sensors.py` checks
   that `sensors.read` is advertised and dispatched under the same option, and
   runs its parser against a harness.

Report environmental sensors (temperature, humidity, CO2, tVOC, light and so
on) through `sensors.read`, on every board that has them, so Muse has one
command to call wherever it runs. Don't add them to `device.health`, which is
for the link itself (battery, power, network), or give them a board-specific
command. Return each reading as `{value, unit, age_s}`, as
`sensecap_sensors.c` does, and report a sensor with no recent reading as null.
Read slow or I2C sensors in a background task and have `sensors.read` return
the latest value.

Muse sees the command once the board reconnects with the new firmware. Keep
the management commands that `on_ws_command()` also handles (`device.list_vms`,
`device.set_vm`, `device.reset_vm` and `device.unpair`) out of
`link.register`: `tests/test_link_transport_contract.py` checks they stay
unadvertised.

## Say Muse, never Hatch

Users never see the name Hatch.

- Anything a person reads says Muse, the Muse app, or the Muse's name:
  - screen text and error captions (`CAN'T REACH MUSE`, not `CAN'T REACH HATCH`)
  - settings labels
  - Kconfig prompts and help
  - log lines
  - tool and script output
  - docs
- Don't use `hatch` in a new file name or identifier. Use `muse` or
  `muse_gadget`. The Muse chat code is `components/muse/muse_chat*`.
- The ESP32 account clients use `https://api.muse.ai`, or the `api_url_v2`
  base the app sends during pairing, with bare API paths. They ignore
  `api_url`: only older firmware reads it, and that firmware adds `/hatch/`
  itself. Keep saving it so a device flashed back to older firmware still works.
- `hatch` stays only where the server or the Muse app depends on it. Don't
  rename these:
  - the VM host `hatch.metaaivm.com`
  - the `hatch_refresh:` auth prefix, the `hatch-web` app id and the
    `HatchLink/` user agent
  - pairing labels and ids such as `hatch-link-pairing-v%d`, the `hatch_link`
    model and the `hatch-link:` device id
  - the bug report fields
  - the setup commands (`hatch.token` and the rest) and the `"hatch"` key
    in the status JSON, which the app and older tools use
- Some older identifiers still carry the name (`muse_hatch_*`,
  `MUSE_HATCH_*`, `CONFIG_MUSE_HATCH`). Leave them unless you're asked to
  rename them. Don't copy the name into new code.

## Tests

These are host-side Python unittest harnesses that compile the firmware C/C++
against fakes, with no board or IDF environment required:

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
```

Run one `idf.py build` first: `test_link_discovery` compiles cJSON from
`managed_components/`, and that directory only exists after a build. Set `CC`
or `CXX` to change compilers.

The e-ink navigation tests use the pinned FreeInk source: set
`FREEINK_SOURCE_DIR` to its checkout, or use `managed_components/freeink-sdk`
from a Sticky configure. CI checks out that exact pin for the host tests.

The Muse apps tests (`tests/test_muse_apps_*.py`; see "Muse apps") build Lua
and the apps' sources with ASan and UBSan where the compiler can. The ones that
need cJSON find it through `CJSON_SOURCE_DIR` or `managed_components/`, which
every `tools/muse/board.sh` build deletes: keep a copy of
`managed_components/espressif__cjson/cJSON` and point `CJSON_SOURCE_DIR` at it
(`test_muse_chat_link_errors` and `test_muse_chat_link_note` read only
`managed_components/`, so copy it back there after a board build).

Two tests skip quietly when their inputs are missing; check the summary for
`skipped=`:

- `test_noise_core` links against the host's PSA Crypto library, found with
  `pkg-config mbedcrypto`. Install `libmbedtls-dev` and `pkg-config` on Debian
  or Ubuntu (as CI does), or `mbedtls` and `pkgconf` with Homebrew.
- `test_link_pairing_handshake` builds Mbed TLS from source for its
  real-crypto case, so it needs `IDF_PATH` (set by `export.sh`) or
  `MBEDTLS_SOURCE_DIR`.

## Before you hand back work

1. `idf.py build` (and the board build you touched) passes, with the size check
   under the slot limit.
2. The host tests pass.
3. If you flashed, the boot log reaches `starting`, and the log shows no
   panic or reboot loop.
