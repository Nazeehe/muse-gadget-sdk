#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Send bench keys, then save a screenshot as PNG:
tools/muse/snap.py [--settle S] <port> <keys> <out.png> [reset_wait_s]

Keys are the serial bench hooks in muse_input.c ('a' menu Down, 's' menu Select, ...),
sent 350 ms apart. A '>' starts a console command that runs to the end of the keys,
sent as one line: '>face=thinking' puts the avatar in a mode, '>face=happy' pets it.
Bench touch boards use '>ui=settings' / '>ui=face' / '>ui=clock' to select the tile;
'a/s' are non-touch menu keys, not touch-settings navigation.
A bench build also injects touch through LVGL's input, as a finger would arrive:
'>tap X Y' presses that point for 80 ms, '>longpress X Y' for 900 ms (LVGL's long press
is 400 ms), and '>swipe left' / '>swipe right' drags across the screen along y=233 in
about 240 ms (a left swipe starts at the right). The screen is woken with 'w' first, since
LVGL doesn't read its input while it sleeps on battery, and the picture is taken once the
script has run and 0.6 s more have passed. --settle S sets that wait, in seconds, for any
command (the others take 0.35 s).
Screenshots are off by default; build and flash with MUSE_BENCH=1 tools/muse/board.sh.
Each one ends with a CRC-32 of its pixels, and a capture that doesn't match is retaken
(three tries). With reset_wait_s the board is reset first and given that long to boot.
The PNG is scaled 3x so small screens are readable.

As a module: parse(log) reads a captured stream, capture(ser, keys, settle) does the whole
exchange on an open port, and write_png(path, w, h, raw) saves what they return.
"""
import argparse
import base64
import binascii
import re
import struct
import sys
import time
import zlib

SCALE = 3
TRIES = 3           # a capture whose CRC doesn't match is taken again, this many times in all
PACE = 0.35         # seconds after each key, and after a command that isn't a touch
AFTER_TOUCH = 0.6   # seconds after a touch script has run, before the picture
TOUCH_MS = {'tap': 80, 'longpress': 900, 'swipe': 240}   # how long each script runs (muse_ui.c)
DEFAULT_PER_LINE = 144    # bytes per base64 line; older firmware doesn't say
STALE_MAX = 64      # a USB packet left in the FIFO while no program had the port open
MAX_SIDE = 4096
B64 = frozenset('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/')

# The stream's own lines start at column 0: an app's @app.log echo carries its text after a prefix, so a line
# that only contains "SNAP END" somewhere is never the firmware's. The one tolerance is a stale fragment glued in
# front of the first line read after the port opened (STALE_MAX bytes, no newline).
BEGIN = re.compile(r'^SNAP BEGIN (\d+) (\d+)(?: (\d+))?[ \t]*\r?$', re.M)
BEGIN_STALE = re.compile(r'[^\n]{0,%d}?SNAP BEGIN (\d+) (\d+)(?: (\d+))?[ \t]*\r?$' % STALE_MAX, re.M)
END = re.compile(r'^SNAP END(?: ([0-9a-fA-F]{8}))?[ \t]*\r?$', re.M)
END_LINE = re.compile(rb'^SNAP END(?: [0-9a-fA-F]{8})?[ \t]*\r?\n', re.M)
BEGIN_LINE = re.compile(rb'^SNAP BEGIN (\d+) (\d+)', re.M)
OFF_LINE = re.compile(rb'^SNAP OFF[ \t]*\r?\n', re.M)


class SnapError(Exception):
    """No picture, and why; str() says it."""


def touch_command(keys):
    """The touch command that `keys` ends with ('tap', 'longpress' or 'swipe'), else None."""
    command = keys.partition('>')[2].split(None, 1)
    return command[0] if command and command[0] in TOUCH_MS else None


def prepare(keys, settle=None):
    """-> (keys, wait): what to send and how long to wait before asking for the picture.

    A touch command is read by LVGL, which doesn't run while it is paused (battery sleep), so a 'w' goes in
    front of it, after any keys before it. The wait is the script's length plus AFTER_TOUCH; anything else takes
    PACE. `settle`, from --settle, replaces the wait.
    """
    touch = touch_command(keys)
    if touch:
        plain, sep, line = keys.partition('>')
        keys = plain + 'w' + sep + line
    wait = TOUCH_MS[touch] / 1000 + AFTER_TOUCH if touch else PACE
    return keys, wait if settle is None else settle


def tail(line):
    """The base64 a line ends with. The log shares the port and can come in front of the base64 on its line, a
    character at a time on a UART console, so the data is the end of the line it's on."""
    end = len(line.rstrip('\r'))
    stop = end
    while stop > 0 and end - stop < 2 and line[stop - 1] == '=':
        stop -= 1
    start = stop
    while start > 0 and line[start - 1] in B64:
        start -= 1
    return line[start:end]


def find_begin(log):
    """The match of the stream's SNAP BEGIN line in `log`, or None."""
    return BEGIN.search(log) or BEGIN_STALE.match(log)


def parse(log):
    """-> (w, h, raw, crc_ok) for the stream in `log`, the text read from the port after the 'p'.

    raw is the RGB565 bytes (low byte first, as LVGL keeps them) of w*h pixels, short of w*h*2 when lines were lost.
    crc_ok is False when the CRC on the SNAP END line doesn't match raw, or raw is short of it; firmware that
    doesn't send one has nothing to check, so it is True. ValueError when there is no stream to read: no
    SNAP BEGIN, no SNAP END after it, or sizes that can't be a screen.
    """
    begin = find_begin(log)
    if begin is None:
        raise ValueError('no SNAP BEGIN')
    w, h = int(begin.group(1)), int(begin.group(2))
    per_line = int(begin.group(3)) if begin.group(3) else DEFAULT_PER_LINE
    if not (0 < w <= MAX_SIDE and 0 < h <= MAX_SIDE and 0 < per_line <= MAX_SIDE):
        raise ValueError(f'SNAP BEGIN {w} {h} {per_line}: not a screen')
    end = END.search(log, begin.end())
    if end is None:
        raise ValueError('no SNAP END: the stream was cut short')
    lines = iter(log[begin.end():end.start()].split('\n'))
    raw = bytearray()
    for n in [min(per_line, w * 2 - x) for _ in range(h) for x in range(0, w * 2, per_line)]:
        k = 4 * -(-n // 3)
        for line in lines:
            data = tail(line)
            if len(data) >= k:
                try:
                    raw += base64.b64decode(data[-k:])
                except binascii.Error:
                    continue    # not the line we wanted: the rest of the line's own text got in
                break
    raw = bytes(raw)
    crc_ok = end.group(1) is None or (len(raw) == w * h * 2 and zlib.crc32(raw) == int(end.group(1), 16))
    return w, h, raw, crc_ok


def read_until(ser, timeout, found, log=b''):
    """Reads from `ser` for up to `timeout` seconds, or until the pattern `found` matches a line of what has come
    in. Returns what was read, after `log`."""
    log = bytearray(log)
    end = time.time() + timeout
    while time.time() < end:
        before = len(log)
        log += ser.read(65536)
        if len(log) > before and found.search(log, max(0, before - 64)):
            break
    return bytes(log)


def listen(ser, seconds):
    """Reads and drops what the board sends for `seconds`."""
    end = time.time() + seconds
    while time.time() < end:
        ser.read(65536)


def settle_down(ser, quiet=1.0, limit=40):
    """Reads and drops until the board has been quiet for `quiet` seconds (at most `limit` in all)."""
    last = time.time()
    end = last + limit
    while time.time() < min(last + quiet, end):
        if ser.read(65536):
            last = time.time()


def fetch(ser):
    """Asks for a picture with 'p' and reads it. -> (w, h, raw, crc_ok, why): why is what was wrong, when it isn't
    ok. SnapError when the board has screenshots off or never answers."""
    ser.write(b'p')
    log = read_until(ser, 10, re.compile(END_LINE.pattern + b'|' + OFF_LINE.pattern, re.M))
    begin = BEGIN_LINE.search(log)
    if begin and not END_LINE.search(log):
        # Wait as long as the image takes on a UART console at 115200 (base64, a newline a
        # line, 10 bits a byte): ~8 s for 135x240, ~40 s for the Watcher's 412x412.
        size = int(begin.group(1)) * int(begin.group(2)) * 2 * 4 / 3 * 1.01
        log = read_until(ser, 10 + size * 10 / 115200, END_LINE, log)
    text = log.decode('latin1')
    try:
        w, h, raw, ok = parse(text)
    except ValueError:
        if find_begin(text) is None:
            if OFF_LINE.search(log):
                raise SnapError('screenshots are off in this build: '
                                'MUSE_BENCH=1 tools/muse/board.sh build|flash <board>')
            raise SnapError('no screenshot: the board never answered')
        return 0, 0, b'', False, 'snapshot cut short'
    return w, h, raw, ok, 'crc mismatch'


def capture(ser, keys='', settle=0.35):
    """Sends `keys` to the open port `ser`, waits `settle` seconds after the last key or command, and takes a
    screenshot. -> (w, h, raw): RGB565 bytes, w*h*2 of them. A capture whose CRC doesn't match is taken again
    (only the 'p', not the keys), up to TRIES times in all; SnapError when none is good, or when there is none.

    keys is what the command line takes: keys sent 350 ms apart, then perhaps a '>' with a console command that
    runs to the end of the keys. See prepare() for a touch command's 'w' and its wait.
    """
    listen(ser, 0.3)
    plain, command, line = keys.partition('>')
    for i, key in enumerate(plain):
        ser.write(key.encode())
        time.sleep(settle if i == len(plain) - 1 and not command else PACE)
    if command:
        ser.write(f'>{line}\n'.encode())
        time.sleep(settle)
    why = ''
    for attempt in range(TRIES):
        w, h, raw, ok, why = fetch(ser)
        if ok:
            break
        if attempt + 1 < TRIES:
            print(f'{why}, retaking', file=sys.stderr)
            settle_down(ser)
    else:
        raise SnapError(f'{why} on all {TRIES} tries: the screenshot would be corrupt, so there is none')
    if len(raw) != w * h * 2:
        print(f'{w * h * 2 - len(raw)} bytes lost (a log line broke into the image): the rest is shifted',
              file=sys.stderr)
        raw = raw.ljust(w * h * 2, b'\0')
    return w, h, raw


def write_png(path, w, h, raw):
    """Saves w x h RGB565 pixels (low byte first) as a PNG, SCALE times as big."""
    raw = raw.ljust(w * h * 2, b'\0')
    img = b''
    for y in range(h):
        line = bytearray([0])
        for x in range(w):
            v = struct.unpack_from('<H', raw, (y * w + x) * 2)[0]
            line += bytes([(v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3]) * SCALE
        img += bytes(line) * SCALE

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data))

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * SCALE, h * SCALE, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(img)) + chunk(b'IEND', b''))


def parse_args(argv):
    p = argparse.ArgumentParser(prog='snap.py', description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                usage='%(prog)s [--settle S] port keys out.png [reset_wait_s]')
    p.add_argument('port', help='the board\'s serial port')
    p.add_argument('keys', help="keys, then perhaps '>' and a console command ('' for none)")
    p.add_argument('out', help='where to save the PNG')
    p.add_argument('reset_wait', nargs='?', type=float, metavar='reset_wait_s',
                   help='reset the board first and give it this long to boot')
    p.add_argument('--settle', type=float, metavar='S',
                   help='seconds to wait before the picture, after the last key or command')
    return p.parse_args(argv)


def open_port(port):
    """The port as tools/muse/chat.py opens it: closing drops DTR, and DTR low with RTS high resets the chip,
    so HUPCL goes off and the lines stay as they are."""
    try:
        import serial
    except ImportError:
        sys.exit('pyserial is missing: python3 -m pip install pyserial')
    ser = serial.Serial(port, 115200, timeout=0.2)
    try:
        import termios
        attrs = termios.tcgetattr(ser.fd)
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(ser.fd, termios.TCSANOW, attrs)
    except (ImportError, AttributeError, OSError):
        pass
    return ser


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    ser = open_port(args.port)
    if args.reset_wait is not None:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        listen(ser, args.reset_wait)
    try:
        w, h, raw = capture(ser, *prepare(args.keys, args.settle))
    except SnapError as e:
        sys.exit(str(e))
    write_png(args.out, w, h, raw)
    print(f'{args.out}: {w}x{h}')


if __name__ == "__main__":
    main()
