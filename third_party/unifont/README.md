# GNU Unifont (subset, vendored)

Upstream: https://unifoundry.com/unifont/ — version 16.0.04
(`unifont-16.0.04.hex.gz` and `unifont_upper-16.0.04.hex.gz`, from
https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/). The
compiled font is used under the SIL Open Font License 1.1 (see `OFL.txt`); Unifont is also available under GPLv2+
with the font embedding exception.

`hold peek --image` renders terminal cells with these 8x16 / 16x16 glyphs.
`src/screen/font_data.h` is generated from the hex file by
`scripts/gen_font.py`, which keeps the whole Basic Multilingual Plane (every
modern script; no surrogates or private use) plus, from the upper plane, math
alphanumerics, emoji and pictographs, and symbols for legacy computing
(sextants). In double-width cells, emoji draw from Noto Emoji instead (see
`../noto-emoji/`). The renderer fits each glyph to the cells the terminal
gave it (a 16-pixel glyph in one cell is squeezed, an 8-pixel glyph in two is
centered). Box-drawing, block-element, and Braille characters are drawn
procedurally for pixel-exact joins, with the font as fallback.
