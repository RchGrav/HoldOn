#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <release-tag-or-version> <output-file>" >&2
  exit 2
fi

version="${1#v}"
tag="v${version}"
output_file="$2"

mkdir -p "$(dirname "$output_file")"

tmp="${output_file}.tmp"
if awk -v version="$version" '
  $0 ~ "^##[[:space:]]+" version "([[:space:]-]|$)" {
    found = 1
    print
    next
  }
  found && /^##[[:space:]]+/ {
    exit
  }
  found {
    print
  }
  END {
    if (!found) {
      exit 1
    }
  }
' CHANGELOG.md >"$tmp" && [[ -s "$tmp" ]]; then
  mv "$tmp" "$output_file"
  exit 0
fi

rm -f "$tmp"
{
  printf '## %s\n\n' "$version"
  printf 'Automated release for `%s`.\n\n' "$tag"
  printf 'No matching `CHANGELOG.md` section was found for `%s`, so this release uses generated notes.\n' "$version"
} >"$output_file"
