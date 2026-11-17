# GNU Unifont (subset, vendored)

Upstream: https://unifoundry.com/unifont/ — version 16.0.04
(`unifont-16.0.04.hex.gz`). The compiled font is used under the SIL Open
Font License 1.1 (see `OFL.txt`); Unifont is also available under GPLv2+
with the font embedding exception.

`hold peek --image` renders terminal cells with these 8x16 / 16x16 glyphs.
`src/screen/font_data.h` is generated from the hex file by
`scripts/gen_font.py`, which keeps the blocks terminal programs draw with;
box-drawing, block-element, and Braille characters are drawn procedurally
by the renderer for pixel-exact joins, with the font as fallback.
