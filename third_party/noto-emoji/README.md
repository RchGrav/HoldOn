# Noto Emoji (monochrome, rasterized, vendored)

Upstream: https://github.com/googlefonts/noto-emoji — the monochrome
`NotoEmoji[wght].ttf` as published in the Google Fonts repository,
`ofl/notoemoji/` at commit `8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5`
(SHA-256 `de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551`).
Copyright 2013 Google LLC; used under the SIL Open Font License 1.1 (see
`OFL.txt`). The font file itself is not vendored.

`scripts/gen_font.py --emoji NotoEmoji[wght].ttf ...` rasterizes, from the
font's TrueType outlines and with no font tooling, every emoji the terminal
grid makes double-width (libvterm's `fullwidth.inc`) to a 32x32 anti-aliased
coverage mask (4 bits per pixel) in `src/screen/font_data.h`. `hold peek
--image` draws it in the cell's foreground color, resampled to the cell size;
single-width (text-style) symbols keep their Unifont glyph. Noto's monochrome
design marks colored circles and squares with hatching patterns.
