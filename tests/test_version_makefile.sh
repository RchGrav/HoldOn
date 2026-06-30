#!/usr/bin/env bash
set -Eeuo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cp Makefile VERSION "$tmp/"
want="$(sed -n '1s/[[:space:]]*$//p' VERSION)"
got="$(cd "$tmp" && make -s --no-print-directory print-version)"

if [ "$got" != "$want" ]; then
  printf 'source-tree VERSION mismatch outside git: got %s want %s\n' "$got" "$want" >&2
  exit 1
fi

case "$got" in
  *-dev-dirty)
    printf 'source-tree VERSION outside git must not append -dev-dirty: %s\n' "$got" >&2
    exit 1
    ;;
esac
