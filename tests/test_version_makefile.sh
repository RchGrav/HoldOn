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

# A pushed release tag is the release source of truth. This lets the manual
# Release Bump workflow create vX.Y.Z without first editing VERSION in the
# tree; non-tag builds still derive from VERSION above.
tag_value="$(GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v9.8.7 bash .github/scripts/resolve_version.sh)"
if [ "$tag_value" != "9.8.7" ]; then
  printf 'tag release version mismatch: got %s want 9.8.7\n' "$tag_value" >&2
  exit 1
fi

tag_base="$(GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v9.8.7 bash .github/scripts/resolve_version.sh --base)"
if [ "$tag_base" != "9.8.7" ]; then
  printf 'tag release base mismatch: got %s want 9.8.7\n' "$tag_base" >&2
  exit 1
fi

out="$tmp/github-output"
GITHUB_OUTPUT="$out" GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v9.8.7 bash .github/scripts/resolve_version.sh --github-output
grep -qx 'value=9.8.7' "$out"
grep -qx 'base=9.8.7' "$out"
grep -qx 'tag=v9.8.7' "$out"
