#!/usr/bin/env bash
#
# Sigmund isolated interactive demo.
#
# This script is meant to be readable. Skim it if you want to see how the demo
# works before running the one-line command from the README.
#
# What this script does:
#   1. Creates one temporary directory under ${TMPDIR:-/tmp}.
#   2. Creates a temporary HOME, Sigmund state directory, boot-id file,
#      helper program, and sandboxed sigmund binary inside that directory.
#   3. Uses an existing sigmund from PATH when it matches this demo release. If
#      sigmund is missing or older, downloads the matching release tarball plus
#      SHA256SUMS, verifies the checksum, and extracts the binary into the
#      temporary directory only.
#   4. Runs a narrated walkthrough: start a helper process, inspect it, dump
#      its log, print the stop command, stop and prune it, create an alias,
#      start from that alias, then stop and prune again.
#   5. Refuses to create sudoers entries. The demo binary is intentionally
#      sandboxed, not a secured root-owned install.
#   6. On normal exit or interruption, tries to stop/prune demo runs and then
#      removes the temporary directory.

set -u

# The demo begins by creating a private workspace. Every file the walkthrough
# needs lives under WORK: the fake HOME, the helper program, the Sigmund state,
# and the sandboxed Sigmund binary. Removing WORK removes the demo.
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/sigmund-demo.XXXXXX")" || exit 1
DEMO_HOME="$WORK/home"
DEMO_BIN_DIR="$WORK/bin"
DEMO_SYSTEM_STATE="$WORK/system-state"
DEMO_BOOT_ID="$WORK/boot_id"
SIGMUND="$DEMO_BIN_DIR/sigmund"
RUN_IDS=""

# These settings let the published demo fetch the matching release artifact.
# The release workflow stamps DEMO_VERSION to the release tag in demo.sh.
REPO_OWNER="${SIGMUND_REPO_OWNER:-RchGrav}"
REPO_NAME="${SIGMUND_REPO_NAME:-sigmund}"
GITHUB_BASE="${SIGMUND_GITHUB_BASE:-https://github.com}"
GITHUB_API="${SIGMUND_GITHUB_API:-https://api.github.com}"
DEMO_VERSION="${SIGMUND_DEMO_VERSION:-latest}"

mkdir -p "$DEMO_HOME" "$DEMO_BIN_DIR" "$DEMO_SYSTEM_STATE" || exit 1
printf 'demo-boot-%s\n' "$(date -u +%Y%m%d%H%M%S 2>/dev/null || echo now)" > "$DEMO_BOOT_ID"

# Cleanup is part of the tutorial: Sigmund is useful because a run can be found
# and stopped later. The trap uses that same contract before deleting WORK.
cleanup() {
  rc=$?
  if [ -n "$RUN_IDS" ] && [ -x "$SIGMUND" ]; then
    for id in $RUN_IDS; do
      HOME="$DEMO_HOME" SIGMUND_BOOT_ID_PATH="$DEMO_BOOT_ID" SIGMUND_TEST_SYSTEM_STATE_DIR="$DEMO_SYSTEM_STATE" \
        "$SIGMUND" stop "$id" >/dev/null 2>&1 || true
      HOME="$DEMO_HOME" SIGMUND_BOOT_ID_PATH="$DEMO_BOOT_ID" SIGMUND_TEST_SYSTEM_STATE_DIR="$DEMO_SYSTEM_STATE" \
        "$SIGMUND" prune "$id" >/dev/null 2>&1 || true
    done
  fi
  rm -rf "$WORK"
  exit "$rc"
}
trap cleanup EXIT HUP INT TERM

say() { printf '\n%s\n' "$*"; }
rule() { printf '\n%s\n' '----------------------------------------------------------------'; }

pause() {
  if [ -t 0 ]; then
    printf '\nPress Enter to continue, or q then Enter to quit: '
    IFS= read -r ans || ans=""
    case "$ans" in q|Q) exit 0 ;; esac
  else
    printf '\n[non-interactive shell: continuing]\n'
  fi
}

# These small helpers support the "no install required" path. If Sigmund is not
# already available, the demo downloads a release into WORK and verifies it
# before running it.
need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    printf 'Missing required command: %s\n' "$1" >&2
    return 1
  }
}

download() {
  url=$1
  out=$2
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$out"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$out" "$url"
  else
    printf 'Missing curl or wget.\n' >&2
    return 1
  fi
}

fetch_text() {
  url=$1
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO- "$url"
  else
    printf 'Missing curl or wget.\n' >&2
    return 1
  fi
}

hash_file() {
  path=$1
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
  else
    printf 'Missing sha256sum or shasum.\n' >&2
    return 1
  fi
}

latest_tag() {
  fetch_text "$GITHUB_API/repos/$REPO_OWNER/$REPO_NAME/releases/latest" |
    sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
    head -n 1
}

normalize_tag() {
  case "$1" in
    v*) printf '%s\n' "$1" ;;
    *) printf 'v%s\n' "$1" ;;
  esac
}

normalize_os() {
  case "$(uname -s)" in
    Darwin|darwin) printf '%s\n' macos ;;
    Linux|linux) printf '%s\n' linux ;;
    *) printf 'Unsupported operating system: %s\n' "$(uname -s)" >&2; return 1 ;;
  esac
}

normalize_arch() {
  case "$(uname -m)" in
    x86_64|amd64) printf '%s\n' amd64 ;;
    arm64|aarch64) printf '%s\n' arm64 ;;
    armv7l|armv7*|armhf) printf '%s\n' armhf ;;
    mipsel) printf '%s\n' mipsel ;;
    riscv64) printf '%s\n' riscv64 ;;
    *) printf 'Unsupported CPU architecture: %s\n' "$(uname -m)" >&2; return 1 ;;
  esac
}

detect_libc() {
  if command -v ldd >/dev/null 2>&1; then
    ldd_out=$(ldd --version 2>&1 || true)
    case "$ldd_out" in
      *musl*) printf '%s\n' musl; return 0 ;;
      *GLIBC*|*GNU\ libc*|*glibc*) printf '%s\n' gnu; return 0 ;;
    esac
  fi
  if getconf GNU_LIBC_VERSION >/dev/null 2>&1; then
    printf '%s\n' gnu
    return 0
  fi
  if ls /lib/ld-musl-*.so.1 /usr/lib/ld-musl-*.so.1 >/dev/null 2>&1; then
    printf '%s\n' musl
    return 0
  fi
  printf '%s\n' unknown
}

# The release artifacts use predictable names. This mirrors install.sh so the
# demo can pick the right tarball for Linux/macOS and the current CPU.
select_artifact() {
  version_no_v=$1
  os=$2
  arch=$3
  libc=$4

  if [ "$os" = macos ]; then
    case "$arch" in
      arm64) printf 'sigmund-%s-macos-arm64.tar.gz\n' "$version_no_v" ;;
      amd64) printf 'sigmund-%s-macos-x86_64.tar.gz\n' "$version_no_v" ;;
      *) return 1 ;;
    esac
    return 0
  fi

  case "$arch" in
    amd64|arm64|armhf)
      case "${SIGMUND_FLAVOR:-}" in
        gnu-dynamic) printf 'sigmund-%s-linux-%s-gnu-dynamic.tar.gz\n' "$version_no_v" "$arch" ;;
        gnu-static) printf 'sigmund-%s-linux-%s-gnu-static.tar.gz\n' "$version_no_v" "$arch" ;;
        musl-static) printf 'sigmund-%s-linux-%s-musl-static.tar.gz\n' "$version_no_v" "$arch" ;;
        "")
          case "$libc" in
            gnu) printf 'sigmund-%s-linux-%s-gnu-static.tar.gz\n' "$version_no_v" "$arch" ;;
            musl) printf 'sigmund-%s-linux-%s-musl-static.tar.gz\n' "$version_no_v" "$arch" ;;
            *) return 1 ;;
          esac
          ;;
        *) return 1 ;;
      esac
      ;;
    mipsel|riscv64) printf 'sigmund-%s-linux-%s-musl-static.tar.gz\n' "$version_no_v" "$arch" ;;
    *) return 1 ;;
  esac
}

# Download Sigmund into WORK. The checksum check matters because this path is
# for users who have not installed Sigmund yet.
download_sigmund_release() {
  need_cmd tar || return 1
  need_cmd awk || return 1
  need_cmd sed || return 1

  tag=$DEMO_VERSION
  if [ "$tag" = latest ]; then
    tag=$(latest_tag) || return 1
    [ -n "$tag" ] || return 1
  else
    tag=$(normalize_tag "$tag")
  fi
  version_no_v=${tag#v}
  os=$(normalize_os) || return 1
  arch=$(normalize_arch) || return 1
  libc=
  if [ "$os" = linux ]; then
    libc=$(detect_libc)
  fi
  artifact=$(select_artifact "$version_no_v" "$os" "$arch" "$libc") || return 1

  say "The demo will download a matching Sigmund release binary into the temporary sandbox."
  say "Nothing is installed system-wide."

  archive="$WORK/$artifact"
  sums="$WORK/SHA256SUMS"
  download "$GITHUB_BASE/$REPO_OWNER/$REPO_NAME/releases/download/$tag/$artifact" "$archive" || return 1
  download "$GITHUB_BASE/$REPO_OWNER/$REPO_NAME/releases/download/$tag/SHA256SUMS" "$sums" || return 1

  expected=$(awk -v artifact="$artifact" '$2 == artifact || $2 == "*" artifact { print $1; exit }' "$sums")
  [ -n "$expected" ] || return 1
  actual=$(hash_file "$archive") || return 1
  [ "$actual" = "$expected" ] || {
    printf 'Checksum mismatch for downloaded Sigmund binary.\n' >&2
    return 1
  }

  extract="$WORK/extract"
  mkdir -p "$extract" || return 1
  tar -xzf "$archive" -C "$extract" || return 1
  bin=$(find "$extract" -type f -name sigmund | head -n 1)
  [ -n "$bin" ] || return 1
  cp "$bin" "$SIGMUND" || return 1
  chmod 0755 "$SIGMUND"
}

# When this script is run from a source checkout, it can build the local source
# instead of downloading a release. That keeps the same script useful while
# working on Sigmund itself.
find_repo_root() {
  if [ -n "${SIGMUND_REPO_ROOT:-}" ] && [ -f "$SIGMUND_REPO_ROOT/src/sigmund.c" ]; then
    printf '%s\n' "$SIGMUND_REPO_ROOT"
    return 0
  fi
  for d in "$SCRIPT_DIR/../.." "$SCRIPT_DIR/../../.." "$PWD"; do
    if [ -f "$d/src/sigmund.c" ]; then
      (cd "$d" && pwd -P)
      return 0
    fi
  done
  return 1
}

# Check whether a local binary matches the release this demo was published for.
# This keeps an older installed Sigmund from making the demo look broken.
sigmund_matches_demo_version() {
  bin=$1
  if [ "$DEMO_VERSION" = latest ]; then
    return 0
  fi
  expected=${DEMO_VERSION#v}
  actual=$("$bin" --version 2>/dev/null | sed -n '1s/[[:space:]]*$//p')
  [ "$actual" = "$expected" ]
}

# Choose the Sigmund binary for the walkthrough. The order is:
#   1. a binary explicitly supplied by SIGMUND_BIN;
#   2. sigmund already on PATH, when it matches this demo release;
#   3. a local source build;
#   4. a downloaded release artifact.
# Whichever path wins, the demo runs the sandbox copy in WORK.
prepare_sigmund() {
  if [ -n "${SIGMUND_BIN:-}" ] && [ -x "$SIGMUND_BIN" ]; then
    cp "$SIGMUND_BIN" "$SIGMUND" || return 1
    chmod 0755 "$SIGMUND"
    return 0
  fi

  if command -v sigmund >/dev/null 2>&1; then
    src="$(command -v sigmund)"
    if sigmund_matches_demo_version "$src"; then
      cp "$src" "$SIGMUND" || return 1
      chmod 0755 "$SIGMUND"
      return 0
    fi
    say "Found a different Sigmund on PATH, so the demo will use the matching release binary in its sandbox."
  fi

  repo_root="$(find_repo_root 2>/dev/null || true)"
  if [ -n "$repo_root" ]; then
    if ! command -v cc >/dev/null 2>&1; then
      download_sigmund_release
      return $?
    fi
    cc -std=c11 -Wall -Wextra -O2 -DSIGMUND_TESTING \
      -o "$SIGMUND" "$repo_root/src/sigmund.c" || return 1
    return 0
  fi

  download_sigmund_release
}

# Run Sigmund with the demo HOME and demo system-state path. This keeps the
# walkthrough separate from any normal Sigmund state on the machine.
run_sigmund() {
  HOME="$DEMO_HOME" SIGMUND_BOOT_ID_PATH="$DEMO_BOOT_ID" SIGMUND_TEST_SYSTEM_STATE_DIR="$DEMO_SYSTEM_STATE" \
    "$SIGMUND" "$@"
}

show_cmd() { printf '\n$ %s\n' "$*"; }

prepare_sigmund || exit 1

# The helper is intentionally tiny. It behaves like a long-running server, but
# the code stays readable enough for the demo to show it on screen.
APP="$WORK/demo-helper.sh"
cat > "$APP" <<'SH'
#!/usr/bin/env sh
trap 'echo "demo-helper: received TERM"; exit 0' TERM INT
count=1
echo "demo-helper: starting"
while :; do
  echo "demo-helper: tick ${count}"
  count=$((count + 1))
  sleep 1
done
SH
chmod 0755 "$APP"

rule
say "Sigmund isolated interactive demo"
say "This tutorial runs on Linux and macOS. It narrates every command before running it."
say "Isolation behavior: this demo creates a temporary HOME, boot-id file, system-state directory, helper app, and sandboxed sigmund binary under:"
printf '  %s\n' "$WORK"
say "It does not install Sigmund, edit shell profiles, or write to /etc. Cleanup removes the whole temporary directory."
say "The demo binary in this sandbox is intentionally not a secured installed file. Because sigmund was not installed as a secured file, this demo is refusing to create a sudoers entry."
say "If you install a secured version of the binary and rerun a grant-oriented demo path, Sigmund can demonstrate that managed-sudoers behavior as well."
pause

rule
say "Step 1: inspect the tiny helper program this demo will launch."
show_cmd "sed -n '1,20p' $APP"
sed -n '1,20p' "$APP"
say "Workflow idea: replace this helper with your API server, database, emulator, web dev server, or integration-test dependency."
pause

rule
say "Step 2: start the helper through Sigmund. Stdout is just the run ID; human context goes to stderr."
show_cmd "sigmund $APP"
START_STDOUT="$WORK/start.stdout"
START_STDERR="$WORK/start.stderr"
if ! run_sigmund "$APP" >"$START_STDOUT" 2>"$START_STDERR"; then
  cat "$START_STDERR" >&2
  exit 1
fi
cat "$START_STDOUT"
cat "$START_STDERR" >&2
RUN_ID="$(sed -n '1p' "$START_STDOUT")"
RUN_IDS="$RUN_IDS $RUN_ID"
say "Captured run ID: $RUN_ID"
say "Benefit: scripts can save this one token and manage the whole process group later."
sleep 2
pause

rule
say "Step 3: list recorded runs from the isolated state directory."
show_cmd "sigmund list --iso"
run_sigmund list --iso
pause

rule
say "Step 4: dump the saved log. The helper keeps running while logs are captured."
show_cmd "sigmund dump $RUN_ID | sed -n '1,8p'"
run_sigmund dump "$RUN_ID" | sed -n '1,8p'
say "Benefit: CI can dump logs on failure without keeping a live tail process alive."
pause

rule
say "Step 5: ask Sigmund to print the signal command it would use after validation."
show_cmd "sigmund stop --print $RUN_ID"
run_sigmund stop --print "$RUN_ID"
say "Benefit: stop acts on the recorded process group after validation, not on a hand-copied PID."
pause

rule
say "Step 6: create an alias from the recorded command, then stop and prune the original run."
show_cmd "sigmund alias $RUN_ID demo-helper"
run_sigmund alias "$RUN_ID" demo-helper
show_cmd "sigmund stop $RUN_ID"
run_sigmund stop "$RUN_ID"
sleep 1
show_cmd "sigmund prune $RUN_ID"
run_sigmund prune "$RUN_ID" || true
RUN_IDS=""
say "The alias stores the launch recipe, so future starts can use a human name."
pause

rule
say "Step 7: start the saved alias and inspect it."
show_cmd "sigmund start demo-helper"
ALIAS_STDOUT="$WORK/alias.stdout"
ALIAS_STDERR="$WORK/alias.stderr"
run_sigmund start demo-helper >"$ALIAS_STDOUT" 2>"$ALIAS_STDERR" || { cat "$ALIAS_STDERR" >&2; exit 1; }
cat "$ALIAS_STDOUT"
cat "$ALIAS_STDERR" >&2
ALIAS_ID="$(sed -n '1p' "$ALIAS_STDOUT")"
RUN_IDS="$RUN_IDS $ALIAS_ID"
sleep 2
show_cmd "sigmund list"
run_sigmund list
show_cmd "sigmund dump $ALIAS_ID | sed -n '1,8p'"
run_sigmund dump "$ALIAS_ID" | sed -n '1,8p'
pause

rule
say "Step 8: clean up the alias-started run."
show_cmd "sigmund stop demo-helper"
run_sigmund stop demo-helper || true
sleep 1
show_cmd "sigmund prune $ALIAS_ID"
run_sigmund prune "$ALIAS_ID" || true
RUN_IDS=""
show_cmd "sigmund list"
run_sigmund list
pause

rule
say "Step 9: sudoers grant behavior is intentionally not performed in this sandbox."
say "Managed sudoers grants are privileged policy. This demo binary lives in a temporary, user-writable directory, so it is not a secured installed file. The demo therefore refuses to create a sudoers entry."
say "A secured install means a regular root-owned sigmund binary with group/world writes disabled and no whitespace in the path. With such an install, Sigmund can demonstrate root-managed grant behavior; this isolated script avoids touching /etc by design."
pause

rule
say "Demo complete. Cleanup is running now; temporary HOME, state, helper app, and sandboxed binary will be removed."
