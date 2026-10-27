# Muse app fonts

`apps_digits_48.c` and `apps_digits_96.c` are the big digit fonts of Muse apps
(`digits48` and `digits96` in the `ui` module), for clocks and timers. Each
defines one `const lv_font_t`, `apps_digits_48` and `apps_digits_96`, and holds
the glyphs of `0123456789:.-+% ` (the digits, colon, full stop, minus, plus,
percent and space). Any other character falls back to `lv_font_montserrat_28`,
the 28 px text font. They are Montserrat Medium drawn at 48 and 96 px, 4 bits per
pixel, uncompressed: about 6 KB and 22 KB of flash. They are built only with
`CONFIG_MUSE_APPS`, and the linker keeps their data only once something refers
to it.

The digits are tabular, so a centred countdown doesn't shift as its figures
change. Montserrat's own are proportional (at 96 px a `1` advances 36 px and a
`0` 64 px). Here `0` to `9` all advance by the widest one's width (32.1 px at 48,
64.25 px at 96), each digit centred in it, and kerning is off. Only the digits'
advances and offsets differ from Montserrat's; the bitmaps are Montserrat's.

`tools/muse/gen_app_fonts.sh` regenerates both from
`../../eink/fonts/montserrat.ttf`, the font the e-ink UI draws with. It needs
npx (Node.js), which fetches `lv_font_conv` 1.5.3, and python3, which runs
`tools/muse/tabular_digits.py` on the result. The `Opts:` line at the top of
each file holds the arguments `lv_font_conv` ran with.

Montserrat is Copyright 2011 The Montserrat Project Authors
(https://github.com/JulietaUla/Montserrat), licensed under the SIL Open Font
License, version 1.1. [`OFL.txt`](OFL.txt) holds its copyright line and the
license text, as LVGL v9.5.0 ships them. These files are a conversion to bitmaps
of an unaltered subset of Montserrat Medium, the file LVGL v9.5.0 builds its own
fonts from, with the digits' advances and offsets changed as above.
