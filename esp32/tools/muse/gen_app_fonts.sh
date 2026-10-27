#!/bin/sh
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

# Regenerates components/muse/apps/fonts/apps_digits_48.c and apps_digits_96.c,
# the big digits of Muse apps (font = "digits48" and "digits96" in the ui
# module): Montserrat Medium at 48 and 96 px, 4 bits per pixel, for "0-9:.-+% "
# and nothing else. Any other character falls back to the 28 px text font. The
# digits are made tabular (tabular_digits.py) and kerning is off, so a countdown
# doesn't shift as its figures change. The source is the e-ink UI's
# montserrat.ttf, a subset of the file LVGL v9.5.0 builds its own fonts from.
# Needs npx (Node.js) and python3.
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/apps/fonts"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# --font is relative to the project, and the Opts: line the tool writes keeps it.
cd "$HERE"
for SIZE in 48 96; do
    npx -y lv_font_conv@1.5.3 --font components/muse/eink/fonts/montserrat.ttf --size "$SIZE" --bpp 4 \
        --format lvgl --lv-font-name "apps_digits_$SIZE" --no-compress --no-kerning \
        --lv-fallback lv_font_montserrat_28 \
        -r 0x20,0x25,0x2B,0x2D,0x2E,0x30-0x3A -o "$TMP/font.c"
    python3 -I tools/muse/tabular_digits.py "$TMP/font.c"
    # The project includes LVGL as "lvgl.h"; the header names no temp paths.
    sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" \
        > "$OUT/apps_digits_$SIZE.c"
    echo "wrote $OUT/apps_digits_$SIZE.c"
done
