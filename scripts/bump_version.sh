#!/usr/bin/env bash
# Compute the next release version from Git tags. Git tags are the release source
# of truth; this script does not edit a VERSION file.
set -euo pipefail
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

usage() {
  cat >&2 <<'EOF'
usage: scripts/bump_version.sh <patch|minor|major|custom> [vX.Y.Z|X.Y.Z]

Prints the next release tag derived from the latest v* tag. Use custom with an
explicit version. This is the local equivalent of the Release Bump workflow's
version calculation; it does not create or push tags.
EOF
  exit 2
}

[ $# -ge 1 ] && [ $# -le 2 ] || usage
bump="$1"
custom="${2:-}"

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
  echo "bump_version: must run inside a git repository" >&2
  exit 1
}

git fetch --tags --quiet 2>/dev/null || true
latest="$(git tag -l 'v*' --sort=-v:refname | head -n1)"
[ -n "$latest" ] || latest="v0.0.0"

case "$bump" in
  patch|minor|major)
    [ -z "$custom" ] || usage
    base="${latest#v}"
    if ! printf '%s\n' "$base" | grep -Eq '^[0-9]+[.][0-9]+[.][0-9]+$'; then
      echo "bump_version: latest tag is not simple semver: $latest" >&2
      exit 1
    fi
    IFS=. read -r major minor patchnum <<EOF
$base
EOF
    case "$bump" in
      patch) patchnum=$((patchnum + 1)) ;;
      minor) minor=$((minor + 1)); patchnum=0 ;;
      major) major=$((major + 1)); minor=0; patchnum=0 ;;
    esac
    next="v${major}.${minor}.${patchnum}"
    ;;
  custom)
    [ -n "$custom" ] || usage
    next="$custom"
    case "$next" in v*) ;; *) next="v$next" ;; esac
    ;;
  *) usage ;;
esac

if ! printf '%s\n' "$next" | grep -Eq '^v[0-9]+[.][0-9]+[.][0-9]+([-.][0-9A-Za-z][0-9A-Za-z.-]*)?$'; then
  echo "bump_version: invalid next tag: $next" >&2
  exit 1
fi
if git rev-parse -q --verify "refs/tags/$next" >/dev/null; then
  echo "bump_version: tag already exists locally: $next" >&2
  exit 1
fi
if git ls-remote --tags origin "refs/tags/$next" 2>/dev/null | grep -q "refs/tags/$next"; then
  echo "bump_version: tag already exists on origin: $next" >&2
  exit 1
fi

printf '%s\n' "$next"
