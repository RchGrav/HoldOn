# libvterm (vendored)

Upstream: https://launchpad.net/libvterm — version 0.3.3, MIT license (see
`LICENSE`). Tarball sha256:
`09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0`.

Copied unmodified: `include/`, `src/*.c`, `src/*.h`, and the pre-generated
`src/*.inc` / `src/encoding/*.inc` tables (no perl step needed). Hold's
`src/screen/` is the only code that includes these headers. To upgrade,
replace these files from a new release tarball and rerun `make check`.

## Local patches

- SGR 2 (faint/dim) is tracked as a pen and cell attribute (`dim`,
  `VTERM_ATTR_DIM`, `VTERM_ATTR_DIM_MASK`); SGR 22 clears it with bold.
  Upstream 0.3.3 ignores SGR 2, so dim text (shell autosuggestions, hint
  bars) was indistinguishable from normal text. Marked `hold patch` in
  `include/vterm.h`, `src/vterm_internal.h`, `src/pen.c`, `src/screen.c`,
  and `src/vterm.c`.
