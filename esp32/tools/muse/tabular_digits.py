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

"""Makes the digits of an lv_font_conv font file tabular, in place.

  tools/muse/tabular_digits.py FONT.c

Montserrat's digits are proportional: at 96 px a 1 advances 36 px and a 0 64 px,
so a centred countdown would shift every second. U+0030 to U+0039 get the
widest digit's advance, and each glyph moves right by half of what its advance
gained. That centres the glyph's own advance in the new one and keeps the
designer's side bearings, which matter for a 1: its advance is lopsided to
balance its flag. The bitmaps are untouched. Run it on a file made with
--no-kerning, since a kerning pair would move a digit by the one before it.
tools/muse/gen_app_fonts.sh does both.
"""

import re
import sys
from pathlib import Path

DIGITS = range(0x30, 0x3A)
MARK = " * Digits U+0030-U+0039: one advance, made by tools/muse/tabular_digits.py\n"

BITMAP_CODE = re.compile(r"^\s*/\* U\+([0-9A-F]+) ", re.M)
OPTS = re.compile(r"^ \* Opts: .*\n", re.M)
GLYPH_TABLE = re.compile(r"(glyph_dsc\[\] = \{\n)(.*?)(\n\};)", re.S)
GLYPH = re.compile(r"(\{\.bitmap_index = \d+, \.adv_w = )(\d+)(, \.box_w = \d+, \.box_h = \d+, \.ofs_x = )(-?\d+)"
                   r"(, \.ofs_y = -?\d+\})")


def tabular_digits(text: str) -> str:
    """`text` with U+0030 to U+0039 given the widest digit's advance, each centred in it. ValueError for a file that
    isn't lv_font_conv's or has no glyph for a digit."""
    table = GLYPH_TABLE.search(text)
    if table is None:
        raise ValueError("no glyph_dsc table")
    codes = [int(code, 16) for code in BITMAP_CODE.findall(text)]
    if codes != sorted(set(codes)):
        raise ValueError("the glyphs are not in code point order")
    missing = [f"U+{code:04X}" for code in DIGITS if code not in codes]
    if missing:
        raise ValueError("no glyph for " + ", ".join(missing))
    entries = list(GLYPH.finditer(table.group(2)))
    if len(entries) != len(codes) + 1:      # glyph id 0 is reserved: glyph ids are 1 + the position in code point order
        raise ValueError(f"{len(entries)} glyph descriptors for {len(codes)} glyphs")
    ids = [codes.index(code) + 1 for code in DIGITS]
    widest = max(int(entries[i].group(2)) for i in ids)
    body = table.group(2)
    for i in reversed(ids):                 # back to front, so the offsets of the entries before stay right
        entry = entries[i]
        # adv_w is in 1/16 px and ofs_x in px: half the gain, in px, rounded half up.
        ofs_x = int(entry.group(4)) + (widest - int(entry.group(2)) + 16) // 32
        if not -128 <= ofs_x <= 127:
            raise ValueError(f"U+{codes[i - 1]:04X} would start {ofs_x} px from its origin")
        body = body[:entry.start()] + f"{entry.group(1)}{widest}{entry.group(3)}{ofs_x}{entry.group(5)}" \
            + body[entry.end():]
    text = text[:table.start(2)] + body + text[table.end(2):]
    if MARK not in text:
        opts = OPTS.search(text)
        if opts is None:
            raise ValueError("no Opts: line")
        text = text[:opts.end()] + MARK + text[opts.end():]
    return text


def main(argv: list) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = Path(argv[1])
    try:
        path.write_text(tabular_digits(path.read_text(encoding="utf-8")), encoding="utf-8")
    except ValueError as error:
        print(f"{path}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
