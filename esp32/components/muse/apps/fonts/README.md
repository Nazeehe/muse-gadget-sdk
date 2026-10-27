# Muse app fonts

`apps_digits_48.c` and `apps_digits_96.c` are the big digit fonts of Muse apps
(`digits48` and `digits96` in the `ui` module), for clocks and timers. Each
defines one `const lv_font_t`, `apps_digits_48` and `apps_digits_96`, and holds
the glyphs of `0123456789:.-+% ` (the digits, colon, full stop, minus, plus,
percent and space) and no others. They are Montserrat Medium drawn at 48 and
96 px, 4 bits per pixel, uncompressed: about 6 KB and 22 KB of flash. They are
built only with `CONFIG_MUSE_APPS`, and the linker keeps their data only once
something refers to it.

`tools/muse/gen_app_fonts.sh` regenerates both from
`../../eink/fonts/montserrat.ttf`, the font the e-ink UI draws with. It needs
npx (Node.js), which fetches `lv_font_conv` 1.5.3. The `Opts:` line at the top
of each file holds the arguments it was made with.

Montserrat is Copyright 2011 The Montserrat Project Authors
(https://github.com/JulietaUla/Montserrat), licensed under the SIL Open Font
License, version 1.1 (https://openfontlicense.org). These files are a
conversion of an unaltered subset of Montserrat Medium, the file LVGL v9.5.0
builds its own fonts from.
