# libvterm (vendored)

Upstream: https://launchpad.net/libvterm — version 0.3.3, MIT license (see
`LICENSE`). Tarball sha256:
`09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0`.

Copied unmodified: `include/`, `src/*.c`, `src/*.h`, and the pre-generated
`src/*.inc` / `src/encoding/*.inc` tables (no perl step needed). Hold's
`src/screen/` is the only code that includes these headers. To upgrade,
replace these files from a new release tarball and rerun `make check`.
